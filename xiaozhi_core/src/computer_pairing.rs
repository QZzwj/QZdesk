use serde_json::{json, Value};
use std::{net::IpAddr, path::PathBuf, sync::{Mutex, OnceLock}, time::{Duration, Instant}};
use tokio::net::UdpSocket;

struct Pending { id: String, code: String, name: String, peer: String, token: String, expires: Instant, status: String, control: bool }
static PENDING: OnceLock<Mutex<Option<Pending>>> = OnceLock::new();
fn state() -> &'static Mutex<Option<Pending>> { PENDING.get_or_init(|| Mutex::new(None)) }
pub fn path() -> PathBuf {
    std::env::var_os("QZDESK_COMPUTER_CONFIG").map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from(std::env::var_os("HOME").unwrap_or_else(|| "/tmp".into())).join(".qzdesk-computer.json"))
}
fn private(ip: IpAddr) -> bool { matches!(ip, IpAddr::V4(ip) if ip.is_private()) }
pub async fn discover() -> anyhow::Result<()> {
    let socket = UdpSocket::bind("0.0.0.0:18765").await?;
    let mut buffer = [0u8; 512];
    loop {
        let (size, peer) = socket.recv_from(&mut buffer).await?;
        if private(peer.ip()) && &buffer[..size] == b"QZDESK_DISCOVER_V1" {
            let port = std::env::var("QZDESK_SKILL_WEB_PORT").ok().and_then(|s| s.parse::<u16>().ok()).unwrap_or(8080);
            let message = json!({"service":"qzdesk-pairing-v1", "name":"QZdesk", "port":port});
            socket.send_to(message.to_string().as_bytes(), peer).await?;
        }
    }
}
pub fn request(peer: IpAddr, body: &Value) -> Result<Value, String> {
    if !private(peer) { return Err("仅允许局域网电脑配对".into()); }
    let token = body["token"].as_str().ok_or("缺少密钥")?;
    if token.len() < 32 || token.len() > 128 || !token.bytes().all(|c| c.is_ascii_alphanumeric() || c == b'_' || c == b'-') { return Err("密钥格式无效".into()); }
    let name = body["name"].as_str().unwrap_or("Windows电脑").chars().filter(|c| !c.is_control()).take(40).collect::<String>();
    let mut guard = state().lock().map_err(|_| "配对状态不可用")?;
    if guard.as_ref().is_some_and(|p| p.expires > Instant::now() && p.status == "pending") { return Err("已有配对请求，请在设备处理或等待过期".into()); }
    let id = uuid::Uuid::new_v4().to_string();
    let code = format!("{:06}", uuid::Uuid::new_v4().as_u128() % 1_000_000);
    *guard = Some(Pending { id: id.clone(), code: code.clone(), name, peer: peer.to_string(), token: token.into(), expires: Instant::now() + Duration::from_secs(90), status: "pending".into(), control: false });
    Ok(json!({"id":id, "code":code, "expires_in":90}))
}
pub fn pending() -> Value {
    let Ok(guard) = state().lock() else { return json!({}); };
    guard.as_ref().filter(|p| p.status == "pending" && p.expires > Instant::now()).map(|p|
        json!({"id":p.id, "code":p.code, "name":p.name, "peer":p.peer})).unwrap_or(json!({}))
}
pub fn status(peer: IpAddr, body: &Value) -> Value {
    let Ok(guard) = state().lock() else { return json!({"status":"unavailable"}); };
    guard.as_ref().filter(|p| p.peer == peer.to_string() && body["id"].as_str() == Some(&p.id)).map(|p|
        json!({"status": if p.expires <= Instant::now() { "expired" } else { &p.status }, "control":p.control})).unwrap_or(json!({"status":"expired"}))
}
pub fn decide(body: &Value) -> Result<Value, String> {
    let mut guard = state().lock().map_err(|_| "配对状态不可用")?;
    let p = guard.as_mut().ok_or("没有配对请求")?;
    if body["id"].as_str() != Some(&p.id) || p.expires <= Instant::now() || p.status != "pending" { return Err("配对请求已失效".into()); }
    if body["allow"].as_bool() == Some(true) {
        let file = path();
        let temp = file.with_extension("tmp");
        use std::os::unix::fs::OpenOptionsExt;
        use std::io::Write;
        let mut output = std::fs::OpenOptions::new().write(true).create(true).truncate(true).mode(0o600).open(&temp).map_err(|_| "无法保存电脑配置")?;
        output.write_all(json!({"endpoint":format!("http://{}:8000/mcp", p.peer), "token":p.token, "control":body["control"].as_bool().unwrap_or(false)}).to_string().as_bytes()).map_err(|_| "无法写入电脑配置")?;
        output.sync_all().map_err(|_| "无法保存电脑配置")?;
        std::fs::rename(temp, file).map_err(|_| "无法保存电脑配置")?;
        p.control = body["control"].as_bool().unwrap_or(false);
        p.status = "approved".into();
    } else { p.status = "rejected".into(); }
    p.token.clear();
    Ok(json!({"status":p.status}))
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn pairing_requires_local_decision_and_expires() {
        let peer: IpAddr = "192.168.1.20".parse().unwrap();
        assert!(request("8.8.8.8".parse().unwrap(), &json!({"token":"a".repeat(43)})).is_err());
        assert!(request(peer, &json!({"token":"short"})).is_err());
        let result = request(peer, &json!({"token":"a".repeat(43), "name":"Test PC"})).unwrap();
        assert_eq!(pending()["id"], result["id"]);
        assert_eq!(status(peer, &result)["status"], "pending");
        assert!(request(peer, &json!({"token":"a".repeat(43)})).is_err());
        assert!(decide(&json!({"id":"wrong", "allow":true})).is_err());
        assert_eq!(decide(&json!({"id":result["id"], "allow":false})).unwrap()["status"], "rejected");
        assert!(pending()["id"].is_null());
        let next = request(peer, &json!({"token":"a".repeat(43)})).unwrap();
        state().lock().unwrap().as_mut().unwrap().expires = Instant::now();
        assert_eq!(status(peer, &next)["status"], "expired");
        assert!(decide(&json!({"id":next["id"], "allow":true})).is_err());
    }
}
