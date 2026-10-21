//! Local-only device-to-device Skill transfer.
//!
//! Discovery uses a small UDP beacon, but the actual bundle never leaves the
//! LAN. Pairing creates a short-lived, single-session token; uploads are
//! chunked and can resume from the byte offset the receiver already persisted.
//! The receiver imports only a complete archive whose SHA-256 matches the
//! sender's manifest.

use crate::controller::CoreCommand;
use crate::mcp_gateway::skill::SkillManager;
use crate::user_data::{read_json, write_json_atomic};
use base64::{Engine as _, engine::general_purpose::URL_SAFE_NO_PAD};
use reqwest::redirect::Policy;
use serde::{Deserialize, Serialize};
use serde_json::{Value, json};
use sha2::{Digest, Sha256};
use std::collections::BTreeMap;
use std::fs::{self, OpenOptions};
use std::net::{IpAddr, Ipv4Addr, SocketAddr};
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex, MutexGuard};
use std::time::{Duration, SystemTime, UNIX_EPOCH};
use tokio::io::AsyncWriteExt;
use tokio::net::UdpSocket;
use tokio::sync::mpsc;
use uuid::Uuid;

const PROTOCOL: &str = "qzdesk.sync";
const DISCOVERY_PREFIX: &str = "QZDESK_DISCOVER_V1 ";
const DEFAULT_DISCOVERY_PORT: u16 = 18_766;
const PAIR_CODE_TTL: u64 = 120;
const TOKEN_TTL: u64 = 10 * 60;
const DISCOVERY_WAIT: Duration = Duration::from_millis(1_800);
const REMOTE_TIMEOUT: Duration = Duration::from_secs(45);
const MAX_BUNDLE_BYTES: usize = 64 * 1024 * 1024;
const MAX_CHUNK_BYTES: usize = 1024 * 1024;
const DEFAULT_CHUNK_BYTES: usize = 256 * 1024;

fn now() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|duration| duration.as_secs())
        .unwrap_or(0)
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(rename_all = "camelCase")]
struct DeviceIdentity {
    id: String,
    name: String,
    secret: String,
    fingerprint: String,
}

impl DeviceIdentity {
    fn public(&self) -> Value {
        json!({
            "id": self.id,
            "name": self.name,
            "fingerprint": self.fingerprint,
        })
    }

    fn valid(&self) -> bool {
        !self.id.is_empty()
            && !self.secret.is_empty()
            && self.fingerprint.len() == 32
            && fingerprint(&self.secret) == self.fingerprint
    }
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct SyncDevice {
    pub id: String,
    pub name: String,
    pub fingerprint: String,
    #[serde(default)]
    pub address: String,
}

impl SyncDevice {
    fn from_json(value: &Value) -> Result<Self, SyncError> {
        let id = value
            .get("id")
            .and_then(Value::as_str)
            .unwrap_or_default()
            .trim()
            .to_string();
        let name = value
            .get("name")
            .and_then(Value::as_str)
            .unwrap_or_default()
            .trim()
            .to_string();
        let fingerprint = value
            .get("fingerprint")
            .and_then(Value::as_str)
            .unwrap_or_default()
            .trim()
            .to_string();
        let address = value
            .get("address")
            .and_then(Value::as_str)
            .unwrap_or_default()
            .trim()
            .to_string();
        if id.is_empty()
            || fingerprint.len() != 32
            || !fingerprint.bytes().all(|byte| byte.is_ascii_hexdigit())
        {
            return Err(SyncError::bad_request("目标设备身份格式无效"));
        }
        Ok(Self {
            id,
            name: if name.is_empty() {
                "QZdesk".to_string()
            } else {
                name
            },
            fingerprint,
            address,
        })
    }

    fn public_value(&self) -> Value {
        json!({
            "id": self.id,
            "name": self.name,
            "fingerprint": self.fingerprint,
            "address": self.address,
        })
    }
}

#[derive(Clone)]
struct PairingCode {
    code: String,
    expires_at: u64,
    wrong_attempts: u8,
}

#[derive(Clone)]
struct AccessGrant {
    source_id: String,
    source_fingerprint: String,
    source_ip: IpAddr,
    target_id: String,
    target_fingerprint: String,
    expires_at: u64,
    upload_id: Option<String>,
}

struct UploadSession {
    source_id: String,
    source_ip: IpAddr,
    target_id: String,
    path: PathBuf,
    size: usize,
    sha256: String,
    chunk_size: usize,
}

#[derive(Clone)]
struct OutboundPairing {
    token: String,
    target: SyncDevice,
    base_url: String,
    bundle: Option<Arc<Vec<u8>>>,
    upload_id: Option<String>,
}

#[derive(Default)]
struct SyncState {
    pairing_code: Option<PairingCode>,
    grants: BTreeMap<String, AccessGrant>,
    uploads: BTreeMap<String, UploadSession>,
    outbound: BTreeMap<String, OutboundPairing>,
    last_event: Option<Value>,
}

#[derive(Clone, Debug)]
pub struct SyncError {
    pub status: u16,
    pub message: String,
    pub conflicts: Vec<String>,
}

impl SyncError {
    fn new(status: u16, message: impl Into<String>) -> Self {
        Self {
            status,
            message: message.into(),
            conflicts: Vec::new(),
        }
    }

    fn bad_request(message: impl Into<String>) -> Self {
        Self::new(400, message)
    }

    fn unauthorized(message: impl Into<String>) -> Self {
        Self::new(401, message)
    }

    fn forbidden(message: impl Into<String>) -> Self {
        Self::new(403, message)
    }

    fn unavailable(message: impl Into<String>) -> Self {
        Self::new(503, message)
    }
}

pub struct DeviceSyncService {
    identity: Arc<DeviceIdentity>,
    manager: Arc<SkillManager>,
    core_tx: mpsc::Sender<CoreCommand>,
    root: PathBuf,
    port: u16,
    state: Arc<Mutex<SyncState>>,
    send_lock: tokio::sync::Mutex<()>,
}

impl DeviceSyncService {
    pub fn start(manager: Arc<SkillManager>, core_tx: mpsc::Sender<CoreCommand>) -> Arc<Self> {
        let root = crate::user_data::file("device-sync");
        let service = Self::new_in(root, manager, core_tx);
        service.start_discovery();
        service
    }

    pub(crate) fn new_in(
        root: PathBuf,
        manager: Arc<SkillManager>,
        core_tx: mpsc::Sender<CoreCommand>,
    ) -> Arc<Self> {
        let _ = fs::create_dir_all(root.join("uploads"));
        cleanup_stale_uploads(&root.join("uploads"));
        let identity = load_identity(&root);
        Arc::new(Self {
            identity: Arc::new(identity),
            manager,
            core_tx,
            root,
            port: discovery_port(),
            state: Arc::new(Mutex::new(SyncState::default())),
            send_lock: tokio::sync::Mutex::new(()),
        })
    }

    fn start_discovery(self: &Arc<Self>) {
        let service = self.clone();
        tokio::spawn(async move {
            loop {
                match UdpSocket::bind(("0.0.0.0", service.port)).await {
                    Ok(socket) => {
                        log::info!(
                            "设备互传发现服务监听 UDP {}（{}）",
                            service.port,
                            service.identity.fingerprint
                        );
                        if let Err(error) = service.serve_discovery(socket).await {
                            log::warn!("设备互传发现服务中断：{}", error);
                        }
                    }
                    Err(error) => {
                        log::warn!(
                            "设备互传发现端口 {} 暂不可用：{}，30 秒后重试",
                            service.port,
                            error
                        );
                    }
                }
                tokio::time::sleep(Duration::from_secs(30)).await;
            }
        });
    }

    async fn serve_discovery(&self, socket: UdpSocket) -> std::io::Result<()> {
        let mut buffer = [0_u8; 2048];
        loop {
            let (length, peer) = socket.recv_from(&mut buffer).await?;
            let packet = &buffer[..length];
            if !packet.starts_with(DISCOVERY_PREFIX.as_bytes()) {
                continue;
            }
            let request_id = String::from_utf8_lossy(&packet[DISCOVERY_PREFIX.len()..])
                .trim()
                .to_string();
            if request_id.is_empty() || request_id.len() > 64 {
                continue;
            }
            let reply = json!({
                "protocol": PROTOCOL,
                "version": 1,
                "requestId": request_id,
                "deviceId": self.identity.id,
                "name": self.identity.name,
                "fingerprint": self.identity.fingerprint,
                "webPort": crate::bridge::web_port(),
                "syncPort": self.port,
            })
            .to_string();
            if reply.len() <= 2048 {
                let _ = socket.send_to(reply.as_bytes(), peer).await;
            }
        }
    }

    pub fn status_json(&self) -> Value {
        let state = self.lock();
        let paired = state
            .outbound
            .values()
            .map(|pairing| pairing.target.public_value())
            .collect::<Vec<_>>();
        json!({
            "ok": true,
            "protocol": PROTOCOL,
            "version": 1,
            "device": self.identity.public(),
            "syncPort": self.port,
            "paired": paired,
            "lastEvent": state.last_event.clone(),
            "limits": {
                "bundleBytes": MAX_BUNDLE_BYTES,
                "chunkBytes": MAX_CHUNK_BYTES,
                "codeSeconds": PAIR_CODE_TTL,
                "tokenSeconds": TOKEN_TTL,
            }
        })
    }

    pub fn issue_pairing_code(&self) -> Value {
        let current = now();
        let code = format!("{:06}", random_u32() % 1_000_000);
        let expires_at = current + PAIR_CODE_TTL;
        let mut state = self.lock();
        state.pairing_code = Some(PairingCode {
            code: code.clone(),
            expires_at,
            wrong_attempts: 0,
        });
        json!({
            "ok": true,
            "code": code,
            "expiresAt": expires_at,
            "expiresIn": PAIR_CODE_TTL,
            "device": self.identity.public(),
        })
    }

    pub async fn discover(&self) -> Result<Value, SyncError> {
        let socket = UdpSocket::bind(("0.0.0.0", 0))
            .await
            .map_err(|error| SyncError::unavailable(format!("无法创建发现连接：{}", error)))?;
        socket
            .set_broadcast(true)
            .map_err(|error| SyncError::unavailable(format!("无法启用局域网广播：{}", error)))?;
        let request_id = Uuid::new_v4().simple().to_string();
        let packet = format!("{}{}", DISCOVERY_PREFIX, request_id);
        let mut targets = vec![SocketAddr::from((Ipv4Addr::BROADCAST, self.port))];
        if let Some(directed) = directed_broadcast(&crate::bridge::device_ip().unwrap_or_default())
        {
            targets.push(SocketAddr::from((directed, self.port)));
        }
        if let Ok(custom) = std::env::var("QZDESK_SYNC_PROBE_ADDR") {
            for value in custom
                .split(',')
                .filter_map(|item| item.trim().parse().ok())
            {
                targets.push(value);
            }
        }
        targets.sort_unstable();
        targets.dedup();
        for target in &targets {
            let _ = socket.send_to(packet.as_bytes(), target).await;
        }

        let deadline = tokio::time::Instant::now() + DISCOVERY_WAIT;
        let mut found = BTreeMap::<String, SyncDevice>::new();
        let mut buffer = [0_u8; 2048];
        loop {
            let remaining = deadline.saturating_duration_since(tokio::time::Instant::now());
            if remaining.is_zero() {
                break;
            }
            let received = tokio::time::timeout(remaining, socket.recv_from(&mut buffer)).await;
            let Ok(Ok((length, peer))) = received else {
                break;
            };
            if !trusted_peer(peer.ip()) {
                continue;
            }
            let Ok(value) = serde_json::from_slice::<Value>(&buffer[..length]) else {
                continue;
            };
            if value.get("protocol").and_then(Value::as_str) != Some(PROTOCOL)
                || value.get("version").and_then(Value::as_u64) != Some(1)
                || value.get("requestId").and_then(Value::as_str) != Some(request_id.as_str())
            {
                continue;
            }
            let id = value
                .get("deviceId")
                .and_then(Value::as_str)
                .unwrap_or_default()
                .to_string();
            let fingerprint = value
                .get("fingerprint")
                .and_then(Value::as_str)
                .unwrap_or_default()
                .to_string();
            if id == self.identity.id
                || id.is_empty()
                || fingerprint.len() != 32
                || !fingerprint.bytes().all(|byte| byte.is_ascii_hexdigit())
            {
                continue;
            }
            let web_port = value.get("webPort").and_then(Value::as_u64).unwrap_or(0);
            if !(1024..=u16::MAX as u64).contains(&web_port) {
                continue;
            }
            let address = format!("{}:{}", peer.ip(), web_port);
            found.insert(
                id.clone(),
                SyncDevice {
                    id,
                    name: value
                        .get("name")
                        .and_then(Value::as_str)
                        .unwrap_or("QZdesk")
                        .to_string(),
                    fingerprint,
                    address,
                },
            );
        }
        Ok(json!({
            "ok": true,
            "devices": found.into_values().collect::<Vec<_>>(),
        }))
    }

    pub async fn pair_remote(&self, address: &str, code: &str) -> Result<Value, SyncError> {
        let code = code.trim();
        if code.len() != 6 || !code.bytes().all(|byte| byte.is_ascii_digit()) {
            return Err(SyncError::bad_request("配对码必须是六位数字"));
        }
        let base_url = normalize_peer_address(address)?;
        let client = remote_client()?;
        let body = json!({
            "code": code,
            "source": {
                "id": self.identity.id,
                "name": self.identity.name,
                "fingerprint": self.identity.fingerprint,
            }
        });
        let (status, value) = remote_json(
            &client,
            reqwest::Method::POST,
            &format!("{}/api/device-sync/target/pair", base_url),
            None,
            Some(&body),
        )
        .await?;
        if status == 401 || status == 403 {
            return Err(SyncError::forbidden(
                value
                    .get("error")
                    .and_then(Value::as_str)
                    .unwrap_or("配对码错误或已过期"),
            ));
        }
        if !(200..300).contains(&status) {
            return Err(SyncError::new(
                status,
                value
                    .get("error")
                    .and_then(Value::as_str)
                    .unwrap_or("目标设备拒绝配对"),
            ));
        }
        let token = value
            .get("token")
            .and_then(Value::as_str)
            .filter(|value| value.len() >= 32)
            .ok_or_else(|| SyncError::unavailable("目标设备没有返回有效令牌"))?;
        let mut target = SyncDevice::from_json(
            value
                .get("device")
                .ok_or_else(|| SyncError::unavailable("目标设备没有返回身份"))?,
        )?;
        target.address = address.trim_end_matches('/').to_string();
        let expires_at = value
            .get("expiresAt")
            .and_then(Value::as_u64)
            .unwrap_or_else(|| now() + TOKEN_TTL);
        let pairing = OutboundPairing {
            token: token.to_string(),
            target: target.clone(),
            base_url,
            bundle: None,
            upload_id: None,
        };
        let mut state = self.lock();
        state.outbound.insert(target.address.clone(), pairing);
        state.last_event = Some(json!({
            "type": "paired",
            "device": target,
            "at": now(),
        }));
        Ok(json!({
            "ok": true,
            "paired": target.public_value(),
            "expiresAt": expires_at,
            "message": "配对成功，令牌仅用于本次同步会话",
        }))
    }

    pub async fn send(&self, address: &str, replace_existing: bool) -> Result<Value, SyncError> {
        let _guard = self.send_lock.lock().await;
        let address = address.trim().trim_end_matches('/').to_string();
        let mut pairing = {
            let state = self.lock();
            state
                .outbound
                .get(&address)
                .cloned()
                .ok_or_else(|| SyncError::forbidden("请先与目标设备完成配对"))?
        };
        if pairing.bundle.is_none() {
            let bundle = self
                .manager
                .export_bundle()
                .map_err(SyncError::bad_request)?;
            if bundle.is_empty() || bundle.len() > MAX_BUNDLE_BYTES {
                return Err(SyncError::bad_request("技能包为空或超过 64 MiB"));
            }
            pairing.bundle = Some(Arc::new(bundle));
            self.update_outbound(&address, |stored| {
                stored.bundle = pairing.bundle.clone();
            });
        }
        let bundle = pairing.bundle.clone().expect("bundle was just loaded");
        let digest = sha256_hex(&bundle);
        let client = remote_client()?;

        let upload_id = loop {
            if pairing.upload_id.is_none() {
                let init = json!({
                    "targetDeviceId": pairing.target.id,
                    "sourceDeviceId": self.identity.id,
                    "size": bundle.len(),
                    "sha256": digest,
                    "chunkSize": DEFAULT_CHUNK_BYTES,
                });
                let (status, value) = remote_json(
                    &client,
                    reqwest::Method::POST,
                    &format!("{}/api/device-sync/target/upload/init", pairing.base_url),
                    Some(&pairing.token),
                    Some(&init),
                )
                .await?;
                if !(200..300).contains(&status) {
                    return Err(remote_error(status, value, "目标设备拒绝创建同步会话"));
                }
                let upload_id = value
                    .get("uploadId")
                    .and_then(Value::as_str)
                    .filter(|value| !value.is_empty())
                    .ok_or_else(|| SyncError::unavailable("目标设备没有返回同步会话"))?
                    .to_string();
                pairing.upload_id = Some(upload_id.clone());
                self.update_outbound(&address, |stored| {
                    stored.upload_id = Some(upload_id);
                });
            }

            let upload_id = pairing.upload_id.clone().expect("upload id is present");
            match self.remote_offset(&client, &pairing, &upload_id).await {
                Ok(offset) => break (upload_id, offset),
                Err(error) if error.status == 404 => {
                    pairing.upload_id = None;
                    self.update_outbound(&address, |stored| stored.upload_id = None);
                }
                Err(error) => return Err(error),
            }
        };
        let (upload_id, mut offset) = upload_id;
        if offset > bundle.len() {
            self.update_outbound(&address, |stored| stored.upload_id = None);
            return Err(SyncError::bad_request("目标设备的分块偏移超出技能包大小"));
        }
        while offset < bundle.len() {
            let end = (offset + DEFAULT_CHUNK_BYTES).min(bundle.len());
            let chunk = bundle[offset..end].to_vec();
            let response = client
                .put(format!(
                    "{}/api/device-sync/target/upload/{}/chunk?offset={}",
                    pairing.base_url, upload_id, offset
                ))
                .bearer_auth(&pairing.token)
                .header("Content-Type", "application/octet-stream")
                .body(chunk)
                .send()
                .await
                .map_err(network_error)?;
            let status = response.status().as_u16();
            let value = response_json(response).await;
            if !(200..300).contains(&status) {
                return Err(remote_error(status, value, "分块上传失败，可重试续传"));
            }
            let received = value
                .get("received")
                .and_then(Value::as_u64)
                .ok_or_else(|| SyncError::unavailable("目标设备没有确认接收偏移"))?
                as usize;
            if received <= offset || received > bundle.len() {
                return Err(SyncError::unavailable("目标设备返回的接收偏移无效"));
            }
            offset = received;
        }

        let finish = json!({
            "replaceExisting": replace_existing,
        });
        let (status, value) = remote_json(
            &client,
            reqwest::Method::POST,
            &format!(
                "{}/api/device-sync/target/upload/{}/finish",
                pairing.base_url, upload_id
            ),
            Some(&pairing.token),
            Some(&finish),
        )
        .await?;
        if status == 409 {
            let conflicts = value
                .get("conflicts")
                .and_then(Value::as_array)
                .map(|items| {
                    items
                        .iter()
                        .filter_map(Value::as_str)
                        .map(str::to_string)
                        .collect::<Vec<_>>()
                })
                .unwrap_or_default();
            return Err(SyncError {
                status,
                message: value
                    .get("error")
                    .and_then(Value::as_str)
                    .unwrap_or("目标设备存在同名 Skill")
                    .to_string(),
                conflicts,
            });
        }
        if !(200..300).contains(&status) {
            if status == 422 {
                self.update_outbound(&address, |stored| stored.upload_id = None);
            }
            return Err(remote_error(status, value, "目标设备导入失败"));
        }

        self.update_outbound(&address, |stored| {
            stored.bundle = None;
            stored.upload_id = None;
        });
        let result = value.get("result").cloned().unwrap_or(Value::Null);
        let returned_device = value
            .get("device")
            .cloned()
            .unwrap_or_else(|| pairing.target.public_value());
        self.lock().last_event = Some(json!({
            "type": "synced",
            "device": returned_device,
            "at": now(),
            "result": result,
        }));
        log::info!(
            "Skill bundle synced to {} ({})",
            pairing.target.name,
            pairing.target.fingerprint
        );
        Ok(json!({
            "ok": true,
            "device": returned_device,
            "sha256": digest,
            "bytes": bundle.len(),
            "result": result,
        }))
    }

    async fn remote_offset(
        &self,
        client: &reqwest::Client,
        pairing: &OutboundPairing,
        upload_id: &str,
    ) -> Result<usize, SyncError> {
        let (status, value) = remote_json(
            client,
            reqwest::Method::GET,
            &format!(
                "{}/api/device-sync/target/upload/{}/status",
                pairing.base_url, upload_id
            ),
            Some(&pairing.token),
            None,
        )
        .await?;
        if !(200..300).contains(&status) {
            return Err(remote_error(status, value, "无法读取断点位置"));
        }
        Ok(value
            .get("received")
            .and_then(Value::as_u64)
            .ok_or_else(|| SyncError::unavailable("目标设备没有返回断点位置"))? as usize)
    }

    fn update_outbound(&self, address: &str, update: impl FnOnce(&mut OutboundPairing)) {
        let mut state = self.lock();
        if let Some(pairing) = state.outbound.get_mut(address) {
            update(pairing);
        }
    }

    pub fn target_pair(&self, peer: IpAddr, input: &Value) -> Result<Value, SyncError> {
        if !trusted_peer(peer) {
            return Err(SyncError::forbidden("仅允许局域网设备配对"));
        }
        let code = input
            .get("code")
            .and_then(Value::as_str)
            .unwrap_or_default()
            .trim();
        let source = input
            .get("source")
            .and_then(Value::as_object)
            .ok_or_else(|| SyncError::bad_request("缺少发送端身份"))?;
        let source_id = source
            .get("id")
            .and_then(Value::as_str)
            .unwrap_or_default()
            .trim()
            .to_string();
        let source_fingerprint = source
            .get("fingerprint")
            .and_then(Value::as_str)
            .unwrap_or_default()
            .trim()
            .to_string();
        if source_id.is_empty()
            || source_fingerprint.len() != 32
            || !source_fingerprint
                .bytes()
                .all(|byte| byte.is_ascii_hexdigit())
        {
            return Err(SyncError::bad_request("发送端身份格式无效"));
        }

        let current = now();
        let mut state = self.lock();
        let pending = state
            .pairing_code
            .as_mut()
            .ok_or_else(|| SyncError::forbidden("目标设备尚未生成配对码"))?;
        if pending.expires_at < current {
            state.pairing_code = None;
            return Err(SyncError::forbidden("配对码已过期，请在目标设备重新生成"));
        }
        if pending.code != code {
            pending.wrong_attempts = pending.wrong_attempts.saturating_add(1);
            if pending.wrong_attempts >= 8 {
                state.pairing_code = None;
            }
            return Err(SyncError::forbidden("配对码错误"));
        }
        state.pairing_code = None;
        let token = random_token();
        let expires_at = current + TOKEN_TTL;
        state.grants.insert(
            token.clone(),
            AccessGrant {
                source_id,
                source_fingerprint,
                source_ip: peer,
                target_id: self.identity.id.clone(),
                target_fingerprint: self.identity.fingerprint.clone(),
                expires_at,
                upload_id: None,
            },
        );
        Ok(json!({
            "ok": true,
            "token": token,
            "expiresAt": expires_at,
            "device": self.identity.public(),
        }))
    }

    pub fn upload_init(
        &self,
        peer: IpAddr,
        token: Option<&str>,
        input: &Value,
    ) -> Result<Value, SyncError> {
        let token = token.ok_or_else(|| SyncError::unauthorized("缺少同步令牌"))?;
        let grant = self.authorize(peer, token)?;
        let source_id = input
            .get("sourceDeviceId")
            .and_then(Value::as_str)
            .unwrap_or_default();
        let target_id = input
            .get("targetDeviceId")
            .and_then(Value::as_str)
            .unwrap_or_default();
        if source_id != grant.source_id {
            return Err(SyncError::forbidden("同步令牌与发送端身份不匹配"));
        }
        if target_id != self.identity.id {
            return Err(SyncError::forbidden("同步请求发错了目标设备"));
        }
        let size = input
            .get("size")
            .and_then(Value::as_u64)
            .map(|value| value as usize)
            .filter(|value| (1..=MAX_BUNDLE_BYTES).contains(value))
            .ok_or_else(|| SyncError::bad_request("技能包大小无效或超过 64 MiB"))?;
        let sha256 = input
            .get("sha256")
            .and_then(Value::as_str)
            .unwrap_or_default()
            .to_ascii_lowercase();
        if sha256.len() != 64 || !sha256.bytes().all(|byte| byte.is_ascii_hexdigit()) {
            return Err(SyncError::bad_request("SHA-256 格式无效"));
        }
        let chunk_size = input
            .get("chunkSize")
            .and_then(Value::as_u64)
            .unwrap_or(DEFAULT_CHUNK_BYTES as u64) as usize;
        if !(32 * 1024..=MAX_CHUNK_BYTES).contains(&chunk_size) {
            return Err(SyncError::bad_request("分块大小必须在 32 KiB 到 1 MiB"));
        }

        let upload_id = Uuid::new_v4().simple().to_string();
        let path = self
            .root
            .join("uploads")
            .join(format!("{}.part", upload_id));
        fs::create_dir_all(path.parent().unwrap_or(Path::new(".")))
            .map_err(|error| SyncError::unavailable(format!("无法创建同步目录：{}", error)))?;
        {
            let file = OpenOptions::new()
                .create_new(true)
                .write(true)
                .open(&path)
                .map_err(|error| SyncError::unavailable(format!("无法创建接收文件：{}", error)))?;
            file.sync_all().map_err(|error| {
                SyncError::unavailable(format!("无法持久化接收文件：{}", error))
            })?;
        }

        let mut state = self.lock();
        if let Some(old_id) = state
            .grants
            .get(token)
            .and_then(|grant| grant.upload_id.clone())
        {
            if let Some(old) = state.uploads.remove(&old_id) {
                let _ = fs::remove_file(old.path);
            }
        }
        state.uploads.insert(
            upload_id.clone(),
            UploadSession {
                source_id: grant.source_id.clone(),
                source_ip: peer,
                target_id: self.identity.id.clone(),
                path,
                size,
                sha256,
                chunk_size,
            },
        );
        if let Some(grant) = state.grants.get_mut(token) {
            grant.upload_id = Some(upload_id.clone());
        }
        Ok(json!({
            "ok": true,
            "uploadId": upload_id,
            "received": 0,
            "expiresAt": grant.expires_at,
        }))
    }

    pub fn upload_status(
        &self,
        peer: IpAddr,
        token: Option<&str>,
        upload_id: &str,
    ) -> Result<Value, SyncError> {
        let token = token.ok_or_else(|| SyncError::unauthorized("缺少同步令牌"))?;
        let grant = self.authorize(peer, token)?;
        let (size, received) = {
            let state = self.lock();
            let upload = state
                .uploads
                .get(upload_id)
                .ok_or_else(|| SyncError::new(404, "同步会话不存在或已过期"))?;
            Self::check_upload_owner(upload, &grant, peer)?;
            let received = fs::metadata(&upload.path)
                .map(|metadata| metadata.len() as usize)
                .unwrap_or(0);
            (upload.size, received)
        };
        Ok(json!({
            "ok": true,
            "uploadId": upload_id,
            "received": received,
            "size": size,
        }))
    }

    pub async fn upload_chunk(
        &self,
        peer: IpAddr,
        token: Option<&str>,
        upload_id: &str,
        offset: usize,
        chunk: &[u8],
    ) -> Result<Value, SyncError> {
        let token = token.ok_or_else(|| SyncError::unauthorized("缺少同步令牌"))?;
        let grant = self.authorize(peer, token)?;
        if chunk.is_empty() || chunk.len() > MAX_CHUNK_BYTES {
            return Err(SyncError::bad_request("分块为空或超过 1 MiB"));
        }
        let (path, size, chunk_size, received) = {
            let state = self.lock();
            let upload = state
                .uploads
                .get(upload_id)
                .ok_or_else(|| SyncError::new(404, "同步会话不存在或已过期"))?;
            Self::check_upload_owner(upload, &grant, peer)?;
            let received = fs::metadata(&upload.path)
                .map(|metadata| metadata.len() as usize)
                .unwrap_or(0);
            (
                upload.path.clone(),
                upload.size,
                upload.chunk_size,
                received,
            )
        };
        if offset != received {
            return Err(SyncError::new(
                409,
                format!("分块偏移不连续，目标设备已有 {} 字节", received),
            ));
        }
        if chunk.len() > chunk_size || received.saturating_add(chunk.len()) > size {
            return Err(SyncError::bad_request("分块超过声明的技能包大小"));
        }
        let mut file = tokio::fs::OpenOptions::new()
            .append(true)
            .open(&path)
            .await
            .map_err(|error| SyncError::unavailable(format!("无法打开接收文件：{}", error)))?;
        file.write_all(chunk)
            .await
            .map_err(|error| SyncError::unavailable(format!("写入分块失败：{}", error)))?;
        file.flush()
            .await
            .map_err(|error| SyncError::unavailable(format!("刷新分块失败：{}", error)))?;
        file.sync_data()
            .await
            .map_err(|error| SyncError::unavailable(format!("持久化分块失败：{}", error)))?;
        Ok(json!({
            "ok": true,
            "received": received + chunk.len(),
            "size": size,
        }))
    }

    pub async fn upload_finish(
        &self,
        peer: IpAddr,
        token: Option<&str>,
        upload_id: &str,
        input: &Value,
    ) -> Result<Value, SyncError> {
        let token = token.ok_or_else(|| SyncError::unauthorized("缺少同步令牌"))?;
        let grant = self.authorize(peer, token)?;
        let (path, size, expected_sha) = {
            let state = self.lock();
            let upload = state
                .uploads
                .get(upload_id)
                .ok_or_else(|| SyncError::new(404, "同步会话不存在或已过期"))?;
            Self::check_upload_owner(upload, &grant, peer)?;
            (upload.path.clone(), upload.size, upload.sha256.clone())
        };
        let data = tokio::fs::read(&path)
            .await
            .map_err(|error| SyncError::unavailable(format!("无法读取完整技能包：{}", error)))?;
        if data.len() != size {
            return Err(SyncError::new(
                422,
                format!("技能包尚未传完：{}/{} 字节", data.len(), size),
            ));
        }
        let actual_sha = sha256_hex(&data);
        if !actual_sha.eq_ignore_ascii_case(&expected_sha) {
            let mut state = self.lock();
            if let Some(upload) = state.uploads.remove(upload_id) {
                let _ = fs::remove_file(upload.path);
            }
            if let Some(grant) = state.grants.get_mut(token) {
                grant.upload_id = None;
            }
            return Err(SyncError::new(
                422,
                format!(
                    "SHA-256 校验失败，已丢弃接收文件（收到 {}，应为 {}）",
                    &actual_sha[..12],
                    &expected_sha[..12]
                ),
            ));
        }
        let replace_existing = input
            .get("replaceExisting")
            .and_then(Value::as_bool)
            .unwrap_or(false);
        let import = self.manager.import_bundle(&data, replace_existing).await;
        match import {
            Ok(result) => {
                self.manager.invalidate_index();
                if self.core_tx.try_send(CoreCommand::SkillsSaved).is_err() {
                    log::warn!("Skill 包已同步，但核心会话重建未排队");
                }
                let mut state = self.lock();
                if let Some(upload) = state.uploads.remove(upload_id) {
                    let _ = fs::remove_file(upload.path);
                }
                state.grants.remove(token);
                state.last_event = Some(json!({
                    "type": "received",
                    "from": grant.source_id,
                    "fromFingerprint": grant.source_fingerprint,
                    "at": now(),
                    "result": result,
                }));
                Ok(json!({
                    "ok": true,
                    "device": self.identity.public(),
                    "sha256": actual_sha,
                    "result": result,
                }))
            }
            Err(error) => Err(SyncError {
                status: if error.conflicts.is_empty() { 400 } else { 409 },
                message: error.message,
                conflicts: error.conflicts,
            }),
        }
    }

    fn authorize(&self, peer: IpAddr, token: &str) -> Result<AccessGrant, SyncError> {
        if !trusted_peer(peer) {
            return Err(SyncError::forbidden("仅允许局域网设备同步"));
        }
        let mut state = self.lock();
        let grant = state
            .grants
            .get(token)
            .cloned()
            .ok_or_else(|| SyncError::unauthorized("同步令牌无效、已使用或已过期"))?;
        if grant.expires_at < now() {
            state.grants.remove(token);
            return Err(SyncError::unauthorized("同步令牌已过期，请重新配对"));
        }
        if grant.source_ip != peer {
            return Err(SyncError::forbidden("令牌来源地址与配对时不一致"));
        }
        if grant.target_fingerprint != self.identity.fingerprint {
            return Err(SyncError::forbidden("目标设备身份已变化"));
        }
        Ok(grant)
    }

    fn check_upload_owner(
        upload: &UploadSession,
        grant: &AccessGrant,
        peer: IpAddr,
    ) -> Result<(), SyncError> {
        if upload.source_ip != peer
            || upload.source_id != grant.source_id
            || upload.target_id != grant.target_id
        {
            return Err(SyncError::forbidden("同步会话与当前设备不匹配"));
        }
        Ok(())
    }

    fn lock(&self) -> MutexGuard<'_, SyncState> {
        self.state.lock().unwrap_or_else(|error| error.into_inner())
    }
}

fn remote_client() -> Result<reqwest::Client, SyncError> {
    reqwest::Client::builder()
        .timeout(REMOTE_TIMEOUT)
        .redirect(Policy::none())
        .build()
        .map_err(|error| SyncError::unavailable(format!("无法创建同步连接：{}", error)))
}

async fn remote_json(
    client: &reqwest::Client,
    method: reqwest::Method,
    url: &str,
    token: Option<&str>,
    body: Option<&Value>,
) -> Result<(u16, Value), SyncError> {
    let mut request = client.request(method, url);
    if let Some(token) = token {
        request = request.bearer_auth(token);
    }
    if let Some(body) = body {
        request = request.json(body);
    }
    let response = request.send().await.map_err(network_error)?;
    let status = response.status().as_u16();
    let value = response_json(response).await;
    Ok((status, value))
}

async fn response_json(response: reqwest::Response) -> Value {
    let bytes = response.bytes().await.unwrap_or_default();
    if bytes.len() > 2 * 1024 * 1024 {
        return json!({"error": "目标设备响应过大"});
    }
    serde_json::from_slice(&bytes).unwrap_or_else(|_| {
        let text = String::from_utf8_lossy(&bytes);
        json!({"error": text.trim().chars().take(300).collect::<String>()})
    })
}

fn remote_error(status: u16, value: Value, fallback: &str) -> SyncError {
    SyncError::new(
        status.max(400),
        value
            .get("error")
            .and_then(Value::as_str)
            .unwrap_or(fallback),
    )
}

fn network_error(error: reqwest::Error) -> SyncError {
    if error.is_timeout() {
        SyncError::unavailable("局域网连接超时；再次同步会从已接收位置继续")
    } else if error.is_connect() {
        SyncError::unavailable("无法连接目标设备；请确认两台设备在同一局域网")
    } else {
        SyncError::unavailable(format!("局域网传输失败：{}", error))
    }
}

fn normalize_peer_address(address: &str) -> Result<String, SyncError> {
    let trimmed = address.trim().trim_end_matches('/');
    if trimmed.is_empty() {
        return Err(SyncError::bad_request("请选择目标设备"));
    }
    let with_scheme = if trimmed.contains("://") {
        trimmed.to_string()
    } else {
        format!("http://{}", trimmed)
    };
    let url = url::Url::parse(&with_scheme)
        .map_err(|_| SyncError::bad_request("目标设备地址格式无效"))?;
    if url.scheme() != "http" || !url.username().is_empty() || url.password().is_some() {
        return Err(SyncError::forbidden("只允许无凭据的局域网 HTTP 地址"));
    }
    let host = url
        .host_str()
        .and_then(|value| value.parse::<IpAddr>().ok())
        .ok_or_else(|| SyncError::forbidden("目标设备必须使用局域网 IP 地址"))?;
    if !trusted_peer(host) {
        return Err(SyncError::forbidden("目标地址不在私网或回环范围"));
    }
    if url.port().is_none() {
        return Err(SyncError::bad_request("目标设备地址缺少端口"));
    }
    Ok(url.to_string().trim_end_matches('/').to_string())
}

fn trusted_peer(ip: IpAddr) -> bool {
    match ip {
        IpAddr::V4(ip) => is_private_v4(ip),
        IpAddr::V6(ip) => ip.is_loopback() || (ip.segments()[0] & 0xfe00) == 0xfc00,
    }
}

fn is_private_v4(ip: Ipv4Addr) -> bool {
    let octets = ip.octets();
    ip.is_private()
        || ip.is_loopback()
        || ip.is_link_local()
        || (octets[0] == 100 && (64..=127).contains(&octets[1]))
}

fn directed_broadcast(local_ip: &str) -> Option<Ipv4Addr> {
    let address = local_ip.parse::<Ipv4Addr>().ok()?;
    if !is_private_v4(address) || address.is_loopback() {
        return None;
    }
    let octets = address.octets();
    Some(Ipv4Addr::new(octets[0], octets[1], octets[2], 255))
}

fn discovery_port() -> u16 {
    std::env::var("QZDESK_SYNC_PORT")
        .ok()
        .and_then(|value| value.parse::<u16>().ok())
        .filter(|value| *value >= 1024)
        .unwrap_or(DEFAULT_DISCOVERY_PORT)
}

fn load_identity(root: &Path) -> DeviceIdentity {
    let path = root.join("identity.json");
    if let Some(value) = read_json(&path) {
        if let Ok(identity) = serde_json::from_value::<DeviceIdentity>(value) {
            if identity.valid() {
                return identity;
            }
        }
    }
    let secret = format!("{}{}", Uuid::new_v4().simple(), Uuid::new_v4().simple());
    let fingerprint = fingerprint(&secret);
    let name = std::env::var("QZDESK_DEVICE_NAME")
        .ok()
        .map(|value| value.trim().chars().take(40).collect::<String>())
        .filter(|value| !value.is_empty())
        .unwrap_or_else(|| format!("QZdesk {}", &fingerprint[..6]));
    let identity = DeviceIdentity {
        id: Uuid::new_v4().to_string(),
        name,
        secret,
        fingerprint,
    };
    let value = serde_json::to_value(&identity).unwrap_or(Value::Null);
    if let Err(error) = write_json_atomic(&path, &value) {
        log::warn!("无法持久化设备互传身份：{}", error);
    }
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        let _ = fs::set_permissions(&path, fs::Permissions::from_mode(0o600));
    }
    identity
}

fn cleanup_stale_uploads(dir: &Path) {
    let Ok(entries) = fs::read_dir(dir) else {
        return;
    };
    let cutoff = now().saturating_sub(24 * 60 * 60);
    for entry in entries.flatten() {
        let path = entry.path();
        let old = entry
            .metadata()
            .ok()
            .and_then(|metadata| metadata.modified().ok())
            .and_then(|modified| modified.duration_since(UNIX_EPOCH).ok())
            .map(|duration| duration.as_secs() < cutoff)
            .unwrap_or(true);
        if old {
            let _ = fs::remove_file(path);
        }
    }
}

fn random_u32() -> u32 {
    let id = Uuid::new_v4();
    u32::from_be_bytes(id.as_bytes()[..4].try_into().expect("uuid prefix"))
}

fn random_token() -> String {
    let first = Uuid::new_v4();
    let second = Uuid::new_v4();
    let mut bytes = [0_u8; 32];
    bytes[..16].copy_from_slice(first.as_bytes());
    bytes[16..].copy_from_slice(second.as_bytes());
    URL_SAFE_NO_PAD.encode(bytes)
}

fn fingerprint(secret: &str) -> String {
    sha256_hex(secret.as_bytes())[..32].to_string()
}

fn sha256_hex(data: &[u8]) -> String {
    format!("{:x}", Sha256::digest(data))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::mcp_gateway::skill::SkillManager;
    use std::sync::atomic::{AtomicU32, Ordering};

    static COUNTER: AtomicU32 = AtomicU32::new(0);

    fn service() -> (Arc<DeviceSyncService>, PathBuf) {
        let root = std::env::temp_dir().join(format!(
            "qzdesk-sync-test-{}-{}",
            std::process::id(),
            COUNTER.fetch_add(1, Ordering::Relaxed)
        ));
        let skills = root.join("skills");
        let sync = root.join("sync");
        fs::create_dir_all(&skills).unwrap();
        fs::create_dir_all(&sync).unwrap();
        let manager = Arc::new(SkillManager::for_test_dir(skills));
        let (tx, _rx) = mpsc::channel(4);
        let service = DeviceSyncService::new_in(sync, manager, tx);
        (service, root)
    }

    #[test]
    fn pairing_code_is_single_use_and_token_matches_peer() {
        let (service, root) = service();
        let issued = service.issue_pairing_code();
        let code = issued["code"].as_str().unwrap();
        let peer = "192.168.1.8".parse().unwrap();
        let paired = service
            .target_pair(
                peer,
                &json!({
                    "code": code,
                    "source": {
                        "id": "source-one",
                        "fingerprint": "0123456789abcdef0123456789abcdef"
                    }
                }),
            )
            .unwrap();
        let token = paired["token"].as_str().unwrap();
        assert!(service.upload_status(peer, Some(token), "missing").is_err());
        assert!(
            service
                .target_pair(
                    peer,
                    &json!({
                        "code": code,
                        "source": {
                            "id": "source-one",
                            "fingerprint": "0123456789abcdef0123456789abcdef"
                        }
                    }),
                )
                .is_err()
        );
        assert!(
            service
                .upload_status("192.168.1.9".parse().unwrap(), Some(token), "missing")
                .is_err()
        );
        let _ = fs::remove_dir_all(root);
    }

    #[tokio::test]
    async fn chunk_upload_keeps_resume_offset_and_rejects_bad_hash() {
        let (service, root) = service();
        let issued = service.issue_pairing_code();
        let code = issued["code"].as_str().unwrap();
        let peer = "10.0.0.7".parse().unwrap();
        let paired = service
            .target_pair(
                peer,
                &json!({
                    "code": code,
                    "source": {
                        "id": "source-two",
                        "fingerprint": "fedcba9876543210fedcba9876543210"
                    }
                }),
            )
            .unwrap();
        let token = paired["token"].as_str().unwrap();
        let data = b"not-a-real-zip-but-hash-tested";
        let init = service
            .upload_init(
                peer,
                Some(token),
                &json!({
                    "targetDeviceId": service.identity.id,
                    "sourceDeviceId": "source-two",
                    "size": data.len(),
                    "sha256": "0".repeat(64),
                    "chunkSize": 64 * 1024,
                }),
            )
            .unwrap();
        let upload_id = init["uploadId"].as_str().unwrap();
        let first = service
            .upload_chunk(peer, Some(token), upload_id, 0, &data[..8])
            .await
            .unwrap();
        assert_eq!(first["received"], 8);
        let status = service.upload_status(peer, Some(token), upload_id).unwrap();
        assert_eq!(status["received"], 8);
        service
            .upload_chunk(peer, Some(token), upload_id, 8, &data[8..])
            .await
            .unwrap();
        let error = service
            .upload_finish(peer, Some(token), upload_id, &json!({}))
            .await
            .unwrap_err();
        assert_eq!(error.status, 422);
        assert!(error.message.contains("SHA-256"));
        let _ = fs::remove_dir_all(root);
    }

    #[test]
    fn public_address_must_stay_on_a_private_network() {
        assert!(normalize_peer_address("192.168.1.9:8080").is_ok());
        assert!(normalize_peer_address("127.0.0.1:8080").is_ok());
        assert!(normalize_peer_address("8.8.8.8:8080").is_err());
        assert!(normalize_peer_address("https://192.168.1.9:8080").is_err());
    }
}
