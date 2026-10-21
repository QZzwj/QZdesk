/*! 手机输入桥：设备上没有中文输入法，手机扫码打开网页控制台的对话页，
 *  在手机上打字，核心把它合成语音后走和麦克风一样的通路上行。
 *
 *  这里只回答一个问题：手机该打开哪个地址。地址必须由核心算 —— 它同时知道
 *  局域网 IP 和网页控制台的端口；界面侧再抄一份，换网 / 换端口就会漂。
 */

use std::ffi::CStr;
use std::net::{IpAddr, Ipv4Addr, UdpSocket};

fn ipv4_preference(ip: Ipv4Addr) -> u8 {
    if ip.is_loopback() || ip.is_unspecified() {
        u8::MAX
    } else if ip.is_private() {
        0
    } else if ip.is_link_local() {
        8
    } else {
        4
    }
}

fn interface_preference(name: &str) -> u8 {
    let name = name.to_ascii_lowercase();
    if name.starts_with("wlan")
        || name.starts_with("wlp")
        || name.starts_with("wl")
        || name.starts_with("eth")
        || name.starts_with("en")
        || name.starts_with("usb")
    {
        0
    } else if name.starts_with("docker")
        || name.starts_with("veth")
        || name.starts_with("virbr")
        || name.starts_with("br-")
        || name.starts_with("tun")
        || name.starts_with("tap")
        || name.starts_with("tailscale")
        || name.starts_with("zt")
    {
        20
    } else {
        10
    }
}

fn best_interface_ip_from(mut addresses: Vec<(String, Ipv4Addr)>) -> Option<Ipv4Addr> {
    addresses.retain(|(_, ip)| ipv4_preference(*ip) != u8::MAX);
    addresses.sort_by_key(|(name, ip)| {
        (
            interface_preference(name),
            ipv4_preference(*ip),
            name.to_ascii_lowercase(),
            ip.octets(),
        )
    });
    addresses.first().map(|(_, ip)| *ip)
}

/// Enumerate addresses directly from the LAN interfaces.
///
/// A UDP route only exists after DHCP installs a default route. Some Wi-Fi
/// stacks briefly keep a valid address after losing that route, which is
/// exactly when the phone bridge is most useful for repairing the network.
fn interface_ipv4() -> Option<Ipv4Addr> {
    let mut addresses = Vec::new();
    unsafe {
        let mut head: *mut libc::ifaddrs = std::ptr::null_mut();
        if libc::getifaddrs(&mut head) != 0 {
            return None;
        }

        let mut current = head;
        while !current.is_null() {
            let interface = &*current;
            if !interface.ifa_addr.is_null() {
                let address = &*interface.ifa_addr;
                let required = (libc::IFF_UP | libc::IFF_RUNNING) as libc::c_uint;
                let enabled = interface.ifa_flags & required == required;
                let loopback = interface.ifa_flags & libc::IFF_LOOPBACK as libc::c_uint != 0;
                if enabled && !loopback && address.sa_family as i32 == libc::AF_INET {
                    let inet = &*(interface.ifa_addr as *const libc::sockaddr_in);
                    let ip = Ipv4Addr::from(inet.sin_addr.s_addr.to_ne_bytes());
                    if ipv4_preference(ip) != u8::MAX {
                        let name = if interface.ifa_name.is_null() {
                            String::new()
                        } else {
                            CStr::from_ptr(interface.ifa_name)
                                .to_string_lossy()
                                .into_owned()
                        };
                        addresses.push((name, ip));
                    }
                }
            }
            current = interface.ifa_next;
        }
        libc::freeifaddrs(head);
    }
    best_interface_ip_from(addresses)
}

fn routed_ipv4() -> Option<Ipv4Addr> {
    let socket = UdpSocket::bind("0.0.0.0:0").ok()?;
    socket.connect("1.1.1.1:80").ok()?;
    match socket.local_addr().ok()?.ip() {
        IpAddr::V4(ip) if ipv4_preference(ip) != u8::MAX => Some(ip),
        _ => None,
    }
}

/// 本机在局域网里的地址。
///
/// 连一个 UDP 对端不会真的发包，只是让内核按默认路由挑一个本地地址；没有
/// 默认路由（未联网）时返回 None，让调用方去说"先连网"，而不是显示一个假的
/// 127.0.0.1 —— 手机扫它只会打开手机自己。
pub fn device_ip() -> Option<String> {
    routed_ipv4()
        .or_else(interface_ipv4)
        .map(|ip| ip.to_string())
}

/// 网页控制台的端口：与 `skill_web::run` 同一个环境变量与兜底值。
pub fn web_port() -> u16 {
    std::env::var("QZDESK_SKILL_WEB_PORT")
        .or_else(|_| std::env::var("XIAOZHI_SKILL_WEB_PORT"))
        .ok()
        .and_then(|value| value.parse::<u16>().ok())
        .unwrap_or(8080)
}

/// 手机扫的地址。`#chat` 让控制台直接落在对话页：手机竖屏下默认那屏是技能
/// 列表，扫码的人还得先在下拉里翻到「对话」才够得着输入框。
pub fn url_for(ip: &str, port: u16) -> String {
    format!("http://{}:{}/#chat", ip, port)
}

/// 当前可用的扫码地址；设备没有局域网地址时为 None。
pub fn web_url() -> Option<String> {
    Some(url_for(&device_ip()?, web_port()))
}

/// 手机输入桥的即时应答。
///
/// 这个请求不能等控制器事件循环：设备没联网时，核心会停在 OTA/激活重试阶段，
/// 把请求留在队列里只会让界面超时并误报“核心没回应”。传输层拿到请求就直接
/// 回答当前能不能生成地址，界面才能明确提示用户先连 WiFi。
pub fn response() -> String {
    match web_url() {
        Some(url) => serde_json::json!({"type":"bridge", "ok":true, "url":url}).to_string(),
        None => serde_json::json!({
            "type":"bridge",
            "ok":false,
            "text":"设备当前没有局域网地址，请先在设置中连接 WiFi"
        })
        .to_string(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn url_points_at_the_chat_view() {
        assert_eq!(
            url_for("192.168.1.20", 8080),
            "http://192.168.1.20:8080/#chat"
        );
    }

    #[test]
    fn url_keeps_a_non_default_port() {
        assert_eq!(url_for("10.8.0.5", 8099), "http://10.8.0.5:8099/#chat");
    }

    /// 二维码是设备屏幕上最小的一块：URL 一长，模块数就上去、每格像素变小，
    /// 手机就更难扫。最长的 IPv4 + 5 位端口也要落在 v3（29×29，M 级 42 字节）内。
    #[test]
    fn url_stays_within_a_compact_qr() {
        let url = url_for("192.168.100.200", 65535);
        assert!(url.len() <= 42, "{} 超出 v3 二维码容量", url);
    }

    #[test]
    fn response_is_a_bridge_event() {
        let value: serde_json::Value = serde_json::from_str(&response()).unwrap();
        assert_eq!(value["type"], "bridge");
        assert!(value["ok"].is_boolean());
    }

    #[test]
    fn prefers_physical_private_interfaces_over_virtual_ones() {
        let addresses = vec![
            ("docker0".to_string(), Ipv4Addr::new(172, 17, 0, 1)),
            ("wlan0".to_string(), Ipv4Addr::new(192, 168, 137, 254)),
            ("eth0".to_string(), Ipv4Addr::new(10, 0, 0, 8)),
        ];
        assert_eq!(
            best_interface_ip_from(addresses),
            Some(Ipv4Addr::new(10, 0, 0, 8))
        );
    }

    #[test]
    fn ignores_loopback_and_unspecified_addresses() {
        let addresses = vec![
            ("lo".to_string(), Ipv4Addr::LOCALHOST),
            ("wlan0".to_string(), Ipv4Addr::UNSPECIFIED),
        ];
        assert_eq!(best_interface_ip_from(addresses), None);
    }
}
