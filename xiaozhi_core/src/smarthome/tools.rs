//! 把智能家居中枢挂到 MCP 上：云端 AI 因此也能列设备、下指令。
//!
//! 这两个工具是原生实现（不是子进程脚本）—— 设备表就在进程内存里，走 MCP 直接
//! 读写最省事，也不需要为每台设备生成一份脚本。

use super::SmartHomeHub;
use crate::mcp_gateway::tool::McpTool;
use async_trait::async_trait;
use serde_json::{json, Value};
use std::sync::Arc;

pub struct DeviceListTool {
    hub: Arc<SmartHomeHub>,
}

impl DeviceListTool {
    pub fn new(hub: Arc<SmartHomeHub>) -> Self {
        Self { hub }
    }
}

#[async_trait]
impl McpTool for DeviceListTool {
    fn name(&self) -> &str {
        "device_list"
    }

    fn description(&self) -> &str {
        "列出局域网里的智能家居设备及其当前状态（开关、亮度、是否在线）。当用户问“家里/房间里的灯和插座”“XX 开着吗”时先调用它拿到设备 id。"
    }

    fn input_schema(&self) -> Value {
        json!({ "type": "object", "properties": {} })
    }

    async fn call(&self, _params: Value) -> Result<Value, String> {
        Ok(self.hub.snapshot())
    }
}

pub struct DeviceControlTool {
    hub: Arc<SmartHomeHub>,
}

impl DeviceControlTool {
    pub fn new(hub: Arc<SmartHomeHub>) -> Self {
        Self { hub }
    }
}

#[async_trait]
impl McpTool for DeviceControlTool {
    fn name(&self) -> &str {
        "device_control"
    }

    fn description(&self) -> &str {
        "控制局域网智能家居设备：开/关灯或插座、调节亮度。设备 id 用 device_list 里的值；action 取 on、off、toggle 或 brightness（后者需要 value=0-100）。"
    }

    fn input_schema(&self) -> Value {
        json!({
            "type": "object",
            "required": ["id", "action"],
            "properties": {
                "id": { "type": "string", "description": "设备 id，来自 device_list" },
                "action": { "type": "string", "description": "on / off / toggle / brightness" },
                "value": { "type": "integer", "description": "action=brightness 时的亮度百分比 0-100" }
            }
        })
    }

    async fn call(&self, params: Value) -> Result<Value, String> {
        let id = params
            .get("id")
            .and_then(Value::as_str)
            .ok_or("缺少 id")?;
        let action = params
            .get("action")
            .and_then(Value::as_str)
            .ok_or("缺少 action")?;
        let value = params.get("value").and_then(Value::as_i64);
        self.hub
            .command(id, action, value)
            .await
            .map_err(|error| error.to_string())
    }
}
