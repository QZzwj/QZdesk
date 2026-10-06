use crate::config::Config;
use reqwest::Client;
use serde_json::json;

pub enum ActivationResult {
    Activated,
    NeedActivation(String), // 包含 6 位验证码
    Error(String),
}

pub async fn check_device_activation(config: &Config) -> ActivationResult {
    // 构造 HTTP URL
    // 从配置文件读取
    let http_url = config.ota_url.as_ref();

    /* OTA 走直连，不读系统的 https_proxy：本机代理（v2ray/clash 的 10808 等）
     * 经常是开机时不在监听、或被规则丢包，导致激活握手在 30 秒后超时——而小智
     * OTA 服务器本身在国内直连可达，没必要绕代理。和天气请求一个原则。 */
    let client = Client::builder().no_proxy().build().unwrap_or_else(|_| Client::new());

    log::info!("Checking activation status via HTTP: {}", http_url);

    // 构造请求体
    let body = json!({
        "uuid": config.client_id,
        "application": {
            "name": env!("APP_NAME"),
            "version": env!("APP_VERSION")
        },
        "ota": {},
        "board": {
            "type": env!("BOARD_TYPE"),
            "name": env!("BOARD_NAME")
        }
    });

    // 构造请求
    // 参考 C++ control_center.cpp 中的 headers
    // 不包含 Authorization 和 Protocol-Version
    let response = client
        .post(http_url)
        .header("Device-Id", &config.device_id)
        .header("Content-Type", "application/json")
        .header("User-Agent", "weidongshan1")
        .header("Accept-Language", "zh-CN")
        .json(&body)
        .send()
        .await;

    match response {
        Ok(resp) => {
            if resp.status().is_success() {
                // 解析 JSON
                match resp.json::<serde_json::Value>().await {
                    Ok(json) => {
                        // 检查是否有 "activation" 字段
                        if let Some(activation) = json.get("activation") {
                            if let Some(code) = activation.get("code") {
                                let code_str = code.as_str().unwrap_or("").to_string();
                                return ActivationResult::NeedActivation(code_str);
                            }
                        }
                        // 如果没有 activation 字段，或者字段为空，视为已激活
                        return ActivationResult::Activated;
                    }
                    Err(e) => ActivationResult::Error(format!("JSON parse error: {}", e)),
                }
            } else {
                ActivationResult::Error(format!("HTTP Error: {}", resp.status()))
            }
        }
        Err(e) => {
            /* reqwest's Display text is intentionally short (usually only
             * "error sending request"). Keep the full Debug chain in logs so
             * proxy, DNS, TLS and connection failures are actionable. */
            log::error!("OTA request failed: {:?}", e);
            ActivationResult::Error(format!("Request failed: {:?}", e))
        }
    }
}
