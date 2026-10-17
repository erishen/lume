#!/bin/sh
# 冒烟调试用 wrapper：tee rea 的完整输出到日志，同时透传 MCP stdio
exec /Users/<user>/.nvm/versions/node/v24.12.0/bin/node \
  /Users/<user>/.npm/_npx/dea9475797100b20/node_modules/rea-agents/scripts/rea.mjs mcp 2>&1 \
  | tee -a /tmp/rea-spawn-debug.log
