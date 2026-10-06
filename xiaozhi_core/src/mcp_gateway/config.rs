use serde::{Deserialize, Serialize};
use serde_json::Value;

/// 执行模式 —— 对话语义层面的同步/异步
/// - Sync（默认）：等待执行完成，结果返回给大模型（对话级同步）
/// - Background：立刻返回，后台执行，完成后通过状态机通知队列告知用户（对话级异步）
#[derive(Clone, Debug, Deserialize, Serialize, Default, PartialEq)]
#[serde(rename_all = "lowercase")]
pub enum ExecutionMode {
    #[default]
    Sync,
    Background,
}

/// 传输协议类型
#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(tag = "type", rename_all = "lowercase")]
pub enum ToolTransport {
    /// 子进程 stdin/stdout 模式
    Subprocess {
        executable: String,
        #[serde(default)]
        args: Vec<String>,
    },
    /// HTTP/REST 调用
    Http {
        url: String,
        #[serde(default = "default_http_method")]
        method: String,
    },
    /// TCP Socket JSON 通信
    Tcp {
        address: String,
    },
}

fn default_http_method() -> String {
    "POST".to_string()
}

fn default_timeout() -> u64 {
    5000
}

/// 异步工具执行完成后的通知方式
/// 预留接口，当前仅支持 Disabled，后续可扩展 Webhook / LocalSocket / Mqtt 等
#[derive(Clone, Debug, Deserialize, Serialize, Default, PartialEq)]
#[serde(tag = "type", rename_all = "lowercase")]
pub enum NotifyMethod {
    #[default]
    Disabled,
    // 预留未来的接口：
    // Webhook { url: String },
    // LocalSocket { path: String },
    // Mqtt { topic: String },
}

/// 配置里省略 `input_schema` 时的默认值：一个不需要参数的对象 schema。
fn default_input_schema() -> Value {
    serde_json::json!({ "type": "object", "properties": {} })
}

/// 统一的工具配置
#[derive(Clone, Debug, Deserialize, Serialize)]
pub struct ExternalToolConfig {
    pub name: String,
    pub description: String,

    /// 工具入参 schema。允许在配置里省略：无参数的工具就是空对象。
    /// 省略原本会让整段 MCP 配置解析失败（提示 missing field `input_schema`），
    /// 结果是所有外部工具一起消失。
    #[serde(default = "default_input_schema")]
    pub input_schema: Value,

    /// 对话语义层面的执行模式，默认为 sync
    #[serde(default)]
    pub mode: ExecutionMode,

    /// 统一超时时间（毫秒），默认 5000ms
    #[serde(default = "default_timeout")]
    pub timeout_ms: u64,

    /// 传输协议配置（扁平化到同一层 JSON/TOML）
    #[serde(flatten)]
    pub transport: ToolTransport,

    /// 异步任务完成后的通知方式（仅对 background 模式有效），默认为 disabled
    #[serde(default)]
    pub notify: NotifyMethod,
}
