//! 最小 MQTT 3.1.1 客户端（QoS0）。
//!
//! 为什么不引依赖：固件里多一棵依赖树不划算 —— 这个项目连 HMAC-SHA256 都是手写的。
//! 而智能家居要的只是 QoS0：zigbee2mqtt 网关本来就按这个档位收发状态与指令，
//! 掉一条状态会随下一次上报补回来，掉一条控制指令用户再点一下即可。
//!
//! 支持：CONNECT/CONNACK、SUBSCRIBE/SUBACK、PUBLISH（收发，含 retain 标志）、
//! PINGREQ/PINGRESP、DISCONNECT，以及 TCP 上的任意分包（MQTT 不保证一条报文
//! 一次读完，所以必须自己攒缓冲）。
//!
//! 不支持（都有明确报错，不会悄悄降级）：TLS、QoS1/2、遗嘱消息、持久会话。

use anyhow::{anyhow, bail, Context, Result};
use std::time::Duration;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::TcpStream;

const TYPE_CONNECT: u8 = 1;
const TYPE_CONNACK: u8 = 2;
const TYPE_PUBLISH: u8 = 3;
const TYPE_SUBSCRIBE: u8 = 8;
const TYPE_SUBACK: u8 = 9;
const TYPE_PINGREQ: u8 = 12;
const TYPE_PINGRESP: u8 = 13;
const TYPE_DISCONNECT: u8 = 14;

/// 一次连接里最多攒多少字节未解析数据：报文本身很小，超过这个量说明对面在乱发，
/// 直接断掉比继续吃内存好。
const MAX_BUFFER: usize = 256 * 1024;

/// keep-alive 的默认值：MQTT 规定服务端在 1.5 倍时间内收不到任何报文就断开，
/// 客户端按一半时间发心跳比较稳。
pub const DEFAULT_KEEP_ALIVE: Duration = Duration::from_secs(30);

/// 一条收到的应用消息。
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Message {
    pub topic: String,
    pub payload: Vec<u8>,
    pub retain: bool,
}

impl Message {
    /// payload 当 UTF-8 文本看（智能家居的 payload 基本都是 JSON 或 ON/OFF）。
    pub fn text(&self) -> String {
        String::from_utf8_lossy(&self.payload).into_owned()
    }
}

/// 解析 `mqtt://host:port` / `tcp://host:port` / `host:port` / `host`。
///
/// TLS 明确拒绝而不是降级成明文：用户以为连的是加密端口却发着明文，是最糟的一种。
pub fn parse_broker(url: &str) -> Result<(String, u16)> {
    let trimmed = url.trim();
    if trimmed.is_empty() {
        bail!("未配置 broker 地址");
    }
    let (scheme, rest) = match trimmed.split_once("://") {
        Some((scheme, rest)) => (scheme.to_ascii_lowercase(), rest.to_string()),
        None => (String::from("mqtt"), trimmed.to_string()),
    };
    match scheme.as_str() {
        "mqtt" | "tcp" => {}
        "mqtts" | "ssl" | "tls" => bail!(
            "暂不支持 TLS 的 MQTT（{}），请改用局域网明文端口 mqtt://",
            trimmed
        ),
        other => bail!("不认识的 MQTT 地址前缀: {}://", other),
    }

    let authority = match rest.split_once('/') {
        Some((authority, _path)) => authority,
        None => rest.as_str(),
    };
    if authority.trim().is_empty() {
        bail!("未配置 broker 主机");
    }
    // IPv6 字面量写成 [::1]:1883
    if let Some(rest) = authority.strip_prefix('[') {
        let (host, tail) = rest
            .split_once(']')
            .ok_or_else(|| anyhow!("IPv6 地址缺少右方括号: {}", authority))?;
        let port = match tail.strip_prefix(':') {
            Some(port) => port.parse::<u16>().context("broker 端口不是数字")?,
            None => 1883,
        };
        return Ok((host.to_string(), port));
    }
    match authority.rsplit_once(':') {
        Some((host, port)) if !port.is_empty() => Ok((
            host.to_string(),
            port.parse::<u16>().context("broker 端口不是数字")?,
        )),
        _ => Ok((authority.to_string(), 1883)),
    }
}

/// 剩余长度的 varint 编码（7 位一组，最高位是续接标志）。
fn encode_remaining_length(mut value: usize, out: &mut Vec<u8>) {
    loop {
        let mut byte = (value % 128) as u8;
        value /= 128;
        if value > 0 {
            byte |= 0x80;
        }
        out.push(byte);
        if value == 0 {
            break;
        }
    }
}

/// 解出剩余长度，返回 (值, 用掉的字节数)。
fn decode_remaining_length(buf: &[u8]) -> Option<(usize, usize)> {
    let mut value = 0usize;
    let mut multiplier = 1usize;
    for (index, byte) in buf.iter().enumerate().take(4) {
        value += (byte & 0x7f) as usize * multiplier;
        if byte & 0x80 == 0 {
            return Some((value, index + 1));
        }
        multiplier *= 128;
    }
    None
}

fn encode_str(value: &str, out: &mut Vec<u8>) {
    out.extend_from_slice(&(value.len() as u16).to_be_bytes());
    out.extend_from_slice(value.as_bytes());
}

fn decode_str(buf: &[u8], offset: usize) -> Option<(String, usize)> {
    if offset + 2 > buf.len() {
        return None;
    }
    let len = u16::from_be_bytes([buf[offset], buf[offset + 1]]) as usize;
    let start = offset + 2;
    let end = start + len;
    if end > buf.len() {
        return None;
    }
    Some((String::from_utf8_lossy(&buf[start..end]).into_owned(), end))
}

/// 拼一个报文：固定头 + 剩余长度 + 载荷。
fn encode_packet(kind: u8, flags: u8, payload: &[u8]) -> Vec<u8> {
    let mut out = Vec::with_capacity(payload.len() + 5);
    out.push((kind << 4) | (flags & 0x0f));
    encode_remaining_length(payload.len(), &mut out);
    out.extend_from_slice(payload);
    out
}

/// 从缓冲里试着解一条报文；数据不够时返回 None（调用方继续收）。
/// 返回 (包类型, flags, 载荷, 消费字节数)。
fn decode_packet(buf: &[u8]) -> Option<(u8, u8, Vec<u8>, usize)> {
    if buf.is_empty() {
        return None;
    }
    let kind = buf[0] >> 4;
    let flags = buf[0] & 0x0f;
    let (length, length_bytes) = decode_remaining_length(buf.get(1..)?)?;
    let header = 1 + length_bytes;
    if buf.len() < header + length {
        return None;
    }
    Some((
        kind,
        flags,
        buf[header..header + length].to_vec(),
        header + length,
    ))
}

/// 用一条 TCP 连接说话的最小客户端。
pub struct MqttClient {
    stream: TcpStream,
    buffer: Vec<u8>,
    keep_alive: Duration,
    packet_id: u16,
}

impl MqttClient {
    /// 连上 broker 并完成 CONNECT/CONNACK 握手。
    pub async fn connect(
        url: &str,
        client_id: &str,
        username: &str,
        password: &str,
        keep_alive: Duration,
    ) -> Result<Self> {
        let (host, port) = parse_broker(url)?;
        let stream = tokio::time::timeout(
            Duration::from_secs(6),
            TcpStream::connect((host.as_str(), port)),
        )
        .await
        .map_err(|_| anyhow!("连接 MQTT broker {}:{} 超时", host, port))?
        .with_context(|| format!("连接 MQTT broker {}:{} 失败", host, port))?;
        stream.set_nodelay(true).ok();

        let mut client = Self {
            stream,
            buffer: Vec::new(),
            keep_alive,
            packet_id: 0,
        };
        client.handshake(client_id, username, password).await?;
        Ok(client)
    }

    async fn handshake(&mut self, client_id: &str, username: &str, password: &str) -> Result<()> {
        // 连接标志：用户名 + 密码 + 清除会话
        let mut flags = 0x02u8;
        if !username.is_empty() {
            flags |= 0x80;
        }
        if !password.is_empty() {
            flags |= 0x40;
        }

        let mut payload = Vec::new();
        encode_str("MQTT", &mut payload);
        payload.push(0x04); // 协议级别 3.1.1
        payload.push(flags);
        payload.extend_from_slice(&(self.keep_alive.as_secs().min(u16::MAX as u64) as u16).to_be_bytes());
        encode_str(client_id, &mut payload);
        if !username.is_empty() {
            encode_str(username, &mut payload);
        }
        if !password.is_empty() {
            encode_str(password, &mut payload);
        }

        let packet = encode_packet(TYPE_CONNECT, 0, &payload);
        self.stream
            .write_all(&packet)
            .await
            .context("发送 MQTT CONNECT 失败")?;

        // CONNACK：载荷两字节（会话标志 + 返回码）
        loop {
            match self.read_packet(Duration::from_secs(6)).await? {
                Some((TYPE_CONNACK, _, body)) => {
                    let code = *body.get(1).ok_or_else(|| anyhow!("CONNACK 载荷不完整"))?;
                    return match code {
                        0 => Ok(()),
                        1 => bail!("MQTT broker 拒绝：协议版本不支持"),
                        2 => bail!("MQTT broker 拒绝：客户端标识不合法"),
                        3 => bail!("MQTT broker 拒绝：服务不可用"),
                        4 => bail!("MQTT broker 拒绝了用户名或密码"),
                        5 => bail!("MQTT broker 拒绝：未授权"),
                        other => bail!("MQTT broker 返回未知错误码 {}", other),
                    };
                }
                Some(_) => continue, // 握手前不该有别的包，忽略
                None => bail!("MQTT broker 没有回 CONNACK（连接被关闭？）"),
            }
        }
    }

    /// 订阅一个主题（QoS0），等 SUBACK。
    pub async fn subscribe(&mut self, topic: &str) -> Result<()> {
        self.packet_id = self.packet_id.wrapping_add(1).max(1);
        let expected = self.packet_id;
        let mut payload = Vec::new();
        payload.extend_from_slice(&expected.to_be_bytes());
        encode_str(topic, &mut payload);
        payload.push(0x00); // 请求 QoS0

        let packet = encode_packet(TYPE_SUBSCRIBE, 0x02, &payload);
        self.stream
            .write_all(&packet)
            .await
            .with_context(|| format!("订阅 {} 失败", topic))?;

        loop {
            match self.read_packet(Duration::from_secs(6)).await? {
                Some((TYPE_SUBACK, _, body)) => {
                    let id = u16::from_be_bytes([body[0], body[1]]);
                    if id == expected {
                        // 返回码 0x80 表示失败；QoS0 之外的值也不接受
                        if body.get(2) == Some(&0x00) {
                            return Ok(());
                        }
                        bail!("broker 拒绝了订阅 {}（返回码 {:?}）", topic, body.get(2));
                    }
                }
                Some(_) => continue,
                None => bail!("订阅 {} 时连接被关闭", topic),
            }
        }
    }

    /// 发布一条消息（QoS0）。`retain` 用于让状态在 broker 上留住。
    pub async fn publish(&mut self, topic: &str, payload: &str, retain: bool) -> Result<()> {
        let mut body = Vec::new();
        encode_str(topic, &mut body);
        body.extend_from_slice(payload.as_bytes());
        let packet = encode_packet(TYPE_PUBLISH, if retain { 0x01 } else { 0x00 }, &body);
        self.stream
            .write_all(&packet)
            .await
            .with_context(|| format!("发布到 {} 失败", topic))
    }

    /// 发心跳。
    pub async fn ping(&mut self) -> Result<()> {
        let packet = encode_packet(TYPE_PINGREQ, 0, &[]);
        self.stream.write_all(&packet).await.context("发送 PINGREQ 失败")
    }

    /// 心跳间隔：MQTT 服务端按 1.5 倍 keep-alive 判死，客户端按一半发比较稳。
    pub fn ping_interval(&self) -> Duration {
        (self.keep_alive / 2).max(Duration::from_secs(5))
    }

    /// 等一条应用消息；超时返回 `Ok(None)`，调用方据此发心跳。
    /// 返回 `Ok(None)` 也可能来自"只收到了 PINGRESP/SUBACK 这类控制包"。
    pub async fn next_message(&mut self, timeout: Duration) -> Result<Option<Message>> {
        let deadline = tokio::time::Instant::now() + timeout;
        loop {
            // 先把已经攒下的数据解析干净
            while let Some((kind, flags, body, consumed)) = decode_packet(&self.buffer) {
                self.buffer.drain(..consumed);
                match kind {
                    TYPE_PUBLISH => {
                        let (topic, offset) = decode_str(&body, 0)
                            .ok_or_else(|| anyhow!("PUBLISH 报文里的主题不完整"))?;
                        // QoS>0 时变长头和主题之间还有一个报文标识符
                        let qos = (flags >> 1) & 0x03;
                        let payload_start = if qos > 0 { offset + 2 } else { offset };
                        let payload = body.get(payload_start..).unwrap_or(&[]).to_vec();
                        return Ok(Some(Message {
                            topic,
                            payload,
                            retain: flags & 0x01 == 0x01,
                        }));
                    }
                    TYPE_PINGREQ => {
                        // 服务端偶尔会反过来问一句，回一个 PINGRESP 就好
                        let packet = encode_packet(TYPE_PINGRESP, 0, &[]);
                        self.stream.write_all(&packet).await.ok();
                    }
                    _ => {} // CONNACK/SUBACK/PINGRESP：这里不需要处理
                }
            }

            let now = tokio::time::Instant::now();
            if now >= deadline {
                return Ok(None);
            }
            let slice = tokio::time::timeout(deadline - now, self.read_chunk()).await;
            match slice {
                Ok(Ok(0)) => bail!("MQTT broker 关闭了连接"),
                Ok(Ok(_)) => {}
                Ok(Err(error)) => return Err(error).context("读取 MQTT 数据失败"),
                Err(_) => return Ok(None),
            }
        }
    }

    async fn read_chunk(&mut self) -> Result<usize> {
        let mut chunk = [0u8; 2048];
        let read = self.stream.read(&mut chunk).await?;
        if read > 0 {
            self.buffer.extend_from_slice(&chunk[..read]);
            if self.buffer.len() > MAX_BUFFER {
                bail!("MQTT 缓冲区溢出（{} 字节未解析）", self.buffer.len());
            }
        }
        Ok(read)
    }

    /// 读一条指定类型的报文（握手与订阅回执用）。
    async fn read_packet(&mut self, timeout: Duration) -> Result<Option<(u8, u8, Vec<u8>)>> {
        let deadline = tokio::time::Instant::now() + timeout;
        loop {
            if let Some((kind, flags, body, consumed)) = decode_packet(&self.buffer) {
                self.buffer.drain(..consumed);
                return Ok(Some((kind, flags, body)));
            }
            let now = tokio::time::Instant::now();
            if now >= deadline {
                return Ok(None);
            }
            match tokio::time::timeout(deadline - now, self.read_chunk()).await {
                Ok(Ok(0)) => return Ok(None),
                Ok(Ok(_)) => {}
                Ok(Err(error)) => return Err(error),
                Err(_) => return Ok(None),
            }
        }
    }

    pub async fn disconnect(&mut self) {
        let packet = encode_packet(TYPE_DISCONNECT, 0, &[]);
        let _ = self.stream.write_all(&packet).await;
        let _ = self.stream.shutdown().await;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn broker_url_forms() {
        assert_eq!(parse_broker("mqtt://192.168.1.10:1883").unwrap(), ("192.168.1.10".into(), 1883));
        assert_eq!(parse_broker("tcp://broker.lan").unwrap(), ("broker.lan".into(), 1883));
        assert_eq!(parse_broker("10.0.0.5").unwrap(), ("10.0.0.5".into(), 1883));
        assert_eq!(parse_broker("mqtt://host:8883/").unwrap(), ("host".into(), 8883));
        assert_eq!(parse_broker("[fd00::1]:1883").unwrap(), ("fd00::1".into(), 1883));
        assert!(parse_broker("mqtts://host:8883").is_err());
        assert!(parse_broker("  ").is_err());
        assert!(parse_broker("mqtt://host:notaport").is_err());
    }

    #[test]
    fn remaining_length_round_trip() {
        for value in [0usize, 1, 127, 128, 16_383, 16_384, 2_097_151, 100_000] {
            let mut encoded = Vec::new();
            encode_remaining_length(value, &mut encoded);
            let (decoded, used) = decode_remaining_length(&encoded).unwrap();
            assert_eq!(decoded, value, "value {}", value);
            assert_eq!(used, encoded.len());
        }
    }

    #[test]
    fn decode_needs_the_whole_packet() {
        let packet = encode_packet(TYPE_PUBLISH, 0, b"\x00\x03abcON");
        // 少一个字节就解不出来，多一个字节不影响
        assert!(decode_packet(&packet[..packet.len() - 1]).is_none());
        let (kind, flags, body, consumed) = decode_packet(&packet).unwrap();
        assert_eq!(kind, TYPE_PUBLISH);
        assert_eq!(flags, 0);
        assert_eq!(consumed, packet.len());
        let (topic, offset) = decode_str(&body, 0).unwrap();
        assert_eq!(topic, "abc");
        assert_eq!(&body[offset..], b"ON");
    }

    #[test]
    fn two_packets_in_one_read() {
        let mut stream = encode_packet(TYPE_PUBLISH, 0x01, b"\x00\x02t1ON");
        stream.extend_from_slice(&encode_packet(TYPE_PUBLISH, 0, b"\x00\x02t2OFF"));
        let (_, flags, body, consumed) = decode_packet(&stream).unwrap();
        assert_eq!(flags & 0x01, 0x01, "retain 标志要能读出来");
        assert_eq!(decode_str(&body, 0).unwrap().0, "t1");
        let (_, _, body2, _) = decode_packet(&stream[consumed..]).unwrap();
        assert_eq!(decode_str(&body2, 0).unwrap().0, "t2");
    }

    #[test]
    fn connect_packet_shape() {
        let mut payload = Vec::new();
        encode_str("MQTT", &mut payload);
        payload.push(0x04);
        payload.push(0x02);
        payload.extend_from_slice(&30u16.to_be_bytes());
        encode_str("qzdesk", &mut payload);
        let packet = encode_packet(TYPE_CONNECT, 0, &payload);
        assert_eq!(packet[0], 0x10, "CONNECT 固定头");
        assert_eq!(packet[1] as usize, payload.len(), "剩余长度 = 载荷长度");
        assert_eq!(&packet[2..8], b"\x00\x04MQTT");
        assert_eq!(packet[8], 0x04, "协议级别 3.1.1");
        assert_eq!(packet[9], 0x02, "清理会话");
    }
}
