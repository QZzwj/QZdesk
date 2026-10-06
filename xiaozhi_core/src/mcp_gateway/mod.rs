pub mod config;
pub mod protocol;
pub mod server;
pub mod skill;
pub mod tool;

pub use config::ExternalToolConfig;
pub use server::McpServer;

use tool::DynamicTool;
use skill::{
    SkillListTool, SkillManager, SkillReadTool, SkillReloadTool, SkillSearchTool, SkillStatusTool,
    SkillValidateTool,
};

pub fn init_mcp_gateway(configs: Vec<ExternalToolConfig>, skill_manager: SkillManager) -> McpServer {
    let mut server = McpServer::new();
    server.set_instructions(skill_manager.instructions());
    server.set_skill_manager(skill_manager.clone());
    for config in configs {
        let tool_name = config.name.clone();
        let tool = DynamicTool::new(config);
        server.register_tool(Box::new(tool));
        log::info!("Registered MCP Tool: {}", tool_name);
    }
    // Skills are file-backed and discovered per request, allowing users to
    // add or remove a Skill under /oem/usr/share/xiaozhi/skills at runtime.
    server.register_tool(Box::new(SkillListTool::new(skill_manager.clone())));
    server.register_tool(Box::new(SkillSearchTool::new(skill_manager.clone())));
    server.register_tool(Box::new(SkillReadTool::new(skill_manager.clone())));
    // 诊断三件套：出问题时能用一句话问清楚「技能到底有没有生效」
    server.register_tool(Box::new(SkillStatusTool::new(skill_manager.clone())));
    server.register_tool(Box::new(SkillValidateTool::new(skill_manager.clone())));
    server.register_tool(Box::new(SkillReloadTool::new(skill_manager)));
    log::info!(
        "Registered local Skill tools: skill_list, skill_search, skill_read, skill_status, skill_validate, skill_reload"
    );
    server
}
