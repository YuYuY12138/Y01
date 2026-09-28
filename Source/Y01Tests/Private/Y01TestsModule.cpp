#include "Modules/ModuleManager.h"

// 本阶段仅注册 Automation；不创建世界、监听端口或启动网络夹具。
// 后续网络夹具必须另外检查显式的 -Y01Smoke 参数。
IMPLEMENT_MODULE(FDefaultModuleImpl, Y01Tests);
