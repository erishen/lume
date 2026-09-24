#ifndef IQUEST_H
#define IQUEST_H

/* 投资助手产品 API,注册成本地 HTTP 端点:
 *
 *   GET  /api/reports          -> 周报归档列表(含大小/时间/摘要预览)
 *   GET  /api/reports/<name>   -> 单篇周报正文
 *   GET  /api/settings         -> 审批开关 + provider 当前值
 *   POST /api/settings         -> 写 frameworks/autogen-pse/.env(限两个白名单键)
 *
 * 数据落点可用 IQUEST_REPORTS_DIR / IQUEST_ENV_FILE 环境变量覆盖(本地单机)。
 * 必须在 agenthttpd_run() 之前调用(注册窗口在 fork 前关闭)。
 */
void iquest_register(void);

#endif /* IQUEST_H */