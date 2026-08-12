# YunMusic for TeamSpeak 3

YunMusic 是一个适用于 64 位 TeamSpeak 3 客户端的原生 Win32 音乐控制插件。
它通过 ts3audiobot 控制播放，并使用网易云音乐 API 提供歌曲与歌单搜索、
歌词、封面、扫码登录和每日推荐。

## 功能

- 播放、暂停、下一首、进度跳转和音量控制
- 歌曲搜索、歌单搜索及歌单详情
- 播放队列查看与批量添加
- 同步歌词和可拖动、锁定、调节字号的桌面歌词
- 网易云音乐扫码登录与每日推荐
- 自动保存窗口位置、尺寸和界面偏好
- 兼容 HTTP 与 HTTPS API 服务

## 安装

1. 从 [Releases](https://github.com/shenmu-rua/YunMusicControlPanel/releases/latest)
   下载最新的 `YunMusicPlugin-v<version>-win64.ts3_plugin`。
2. 双击安装包，通过 TeamSpeak Package Installer 完成安装。
3. 编辑 TeamSpeak 插件目录中的 `yunmusic.ini`。
4. 重启 TeamSpeak，在“工具 → 选项 → 插件”中启用 YunMusic。
5. 从插件菜单打开 **Music Panel**。

插件需要能够访问以下两个服务：

- ts3audiobot API
- 网易云音乐 API

## 配置

所有网络地址集中在 `yunmusic.ini` 的 `[network]` 段。两个 API 共用一个
`host`，更换服务器时只需修改这一项。

```ini
[network]
host=localhost
scheme=http
bot_port=58913
netease_port=3000
```

配置项说明：

| 配置项 | 说明 | 默认值 |
| --- | --- | --- |
| `host` | 两个 API 共用的主机名或 IP | `localhost` |
| `scheme` | 请求协议，可设为 `http` 或 `https` | `http` |
| `bot_port` | ts3audiobot API 端口 | `58913` |
| `netease_port` | 网易云音乐 API 端口 | `3000` |

如果两个服务部署在同一台远程服务器，只需修改一行：

```ini
host=your-server.example.com
```

修改配置后请重启 TeamSpeak，使两个 API 客户端同时应用新地址。

完整示例参见 [`yunmusic.ini.example`](yunmusic.ini.example)。

## 账号与隐私

个人网易云账号可在 Music Panel 的“账号”窗口扫码登录。完整 Cookie 使用
Windows DPAPI 加密后保存到插件目录中的 `yunmusic.auth`，不会发送给
ts3audiobot。旧配置中的明文 `music_u` 会在成功加密并确认可回读后自动删除。

实际部署参数保存在本地配置文件中。请勿把包含 Cookie、令牌或其他凭据的
`yunmusic.ini`、`yunmusic.auth` 提交到公开仓库。

## 使用说明

- 搜索框支持 Enter 搜索；方向键可进入结果列表，Enter 可播放选中项。
- 歌单结果可进入曲目详情，逐首播放或把前 15 首加入队列。
- 每日推荐不会自动播放；可以播放单曲或把前 15 首追加到公共 Bot 队列。
- 插件会定期同步 Bot 的真实播放队列。在 TS3 聊天中执行队列命令后，面板也会
  随后更新。
- 桌面歌词未锁定时可拖动，滚轮可调整字号；右键菜单提供字号、位置复位和隐藏。
- 主窗口位置、尺寸、桌面歌词位置、锁定状态和字号会自动保存。
- `poll_interval_ms` 的有效范围为 500–60000 毫秒。

## 从源码构建

环境要求：

- Visual Studio 2022，并安装 **Desktop development with C++**
- CMake 3.10 或更高版本
- 64 位 Windows

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

构建结果位于 `build/Release/`，其中包括：

- `YunMusicPlugin.dll`
- `YunMusicPlugin-v<version>-win64.ts3_plugin`
- `yunmusic.ini.example`
- `YunMusicPlugin.png`

`.ts3_plugin` 安装包根目录包含 `package.ini`，插件 DLL 和运行资源位于
`plugins/` 目录。

## 项目结构

- `src/plugin.c`：TeamSpeak 插件入口、菜单和命令
- `src/ui_win32.c`：Win32 控制面板、桌面歌词和后台任务
- `src/config.c`：统一配置加载、校验和保存
- `src/api_bot.c`：ts3audiobot 状态与控制 API
- `src/api_netease.c`：网易云搜索、歌词、封面与登录 API
- `src/lrc_parser.c`：LRC 解析和歌词定位
- `tests/`：解析、认证、Cookie 和网络安全测试

## 故障排查

- 面板无法取得播放状态：确认 `host`、`scheme` 和 `bot_port` 正确，并检查
  ts3audiobot API 是否允许当前客户端访问。
- 搜索、歌词或扫码登录失败：确认网易云音乐 API 正常运行，并检查
  `netease_port`。
- 修改配置后没有生效：完全退出并重新启动 TeamSpeak。
- 远程 HTTP 服务无法访问：检查服务器安全组、防火墙和端口监听状态；公网部署
  建议使用 HTTPS 和反向代理，不要直接暴露不必要的管理端口。
