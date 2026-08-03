# YunMusic TeamSpeak 3 Plugin

YunMusic 是一个原生 Win32 TeamSpeak 3 客户端插件。它通过
ts3audiobot 控制播放，并通过网易云音乐 API 提供搜索、歌词、封面、
歌单和每日推荐。

## 构建

要求：

- Visual Studio 2022（Desktop development with C++）
- CMake 3.10 或更新版本
- 64 位 TeamSpeak 3 客户端

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

构建结果位于 `build/Release/`。CMake 会把菜单图标和示例配置一起复制
到该目录，并自动生成带版本号的 TeamSpeak 安装包，例如
`YunMusicPlugin-v<version>-win64.ts3_plugin`。

每次修改插件后都应在 `CMakeLists.txt` 中递增补丁版本号；插件内显示的
版本和安装包 `package.ini` 会从该版本自动生成。

## 配置与安装

1. 双击构建目录中的 `.ts3_plugin` 文件并通过 TeamSpeak Package
   Installer 安装。
2. 首次运行后修改插件目录中的 `yunmusic.ini`，配置 `[server].api_url`
   和 `[netease].api_url`。
3. 重启 TeamSpeak，并从插件菜单打开 **Music Panel**。

个人网易云账号从面板的“账号”窗口扫码登录，完整 Cookie 仅使用 Windows
DPAPI 加密保存为插件目录中的 `yunmusic.auth`，不会发送给 Bot。旧配置中的
明文 `music_u` 会在加密写入并回读成功后自动删除。
`poll_interval_ms` 会被限制在 500–60000 毫秒之间。
当前默认 Bot API 为 `http://localhost:58913`；旧版生成的
`localhost:58913` 配置会在加载时自动迁移。
默认网易云 API 为 `http://localhost:3000`；旧版生成的
`localhost:3000` 配置同样会自动迁移。网易云搜索、歌词、封面、个人登录、
登录状态和每日推荐均兼容 HTTP 与 HTTPS API 地址。

## 使用体验

- 主面板支持调整大小，并会保存窗口位置与尺寸。
- 搜索框支持 Enter 搜索、方向键进入结果，结果列表支持 Enter 播放和
  键盘菜单键。
- 每日推荐仅展示当前 Windows 用户账号的结果，不会自动播放；单曲可立即
  播放或加入下一首，“播放全部（前15首）”只追加到公共 Bot 队列。
- 歌单搜索结果双击后进入曲目详情页，可返回搜索结果、逐首播放或把前
  15 首追加到队列，不再自动播放整张歌单。
- 批量添加会针对 Bot 的后进先出队列倒序发送，因此最终仍按详情页从上
  到下播放；Bot 空闲时会先启动列表第 1 首，再加入其余曲目。
- Panel 首次打开会通过 `yun list` 同步 Bot 的真实播放队列，之后每隔数次
  状态轮询刷新一次，因此在 TS3 聊天中手动执行 `yun clear` 也会更新界面。
  Bot 插件当前只返回接下来的 3 首，更多曲目会显示为剩余数量提示。
- 桌面歌词使用半透明深色背景；未锁定时可拖动和滚轮调整字号，左侧可
  快速锁定。右键菜单提供字号、复位位置和隐藏操作。
- 桌面歌词的位置、锁定状态和字号会自动保存。

## 模块

- `src/plugin.c`：TeamSpeak 插件入口、菜单和命令。
- `src/ui_win32.c`：Win32 控制面板、歌词与后台任务。
- `src/api_bot.c`：ts3audiobot 状态和控制 API。
- `src/api_netease.c`：网易云搜索、歌词、封面与登录 API。
- `src/lrc_parser.c`：LRC 解析和歌词定位。
