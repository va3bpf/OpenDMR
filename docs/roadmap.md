# 开发计划

OpenDMR 是业余时间做的项目，下面只是打算做的事和大致先后，**没有时间表**，顺序也可能变。欢迎在
[Issues](https://github.com/va3bpf/OpenDMR/issues) 里提建议，或者直接提交 PR。

English: [roadmap.en.md](roadmap.en.md)

## 近期

**APRS**（信标频率、APRS 走 DMR、卫星 APRS 已在 0.6.0 完成）

- 在空中实测：信标用 APRS 解码器 / aprs.fi 接收，QSY 信息，经卫星转发
- APRS 走 DMR：确认网关（BrandMeister）能解出 LRRP 报告（[#72](https://github.com/va3bpf/OpenDMR/pull/72)）

**收音机**

- 切换频率后偶尔没有声音（[#65](https://github.com/va3bpf/OpenDMR/issues/65)）

**写频软件（OpenDMR CPS）**

- 卫星 APRS：按卫星一键建同名的 APRS 配置（路径如 ISS 的 `RS0ISS`、SONATE-2 的 `DP0SNX`）
- APRS 图标从列表里选，不用手输字符（[CPS #8](https://github.com/va3bpf/opendmr-cps/pull/8)）
- 编辑 DTMF 等其他现在原样写回的部分

**说明书**

- 其余截图按新的 S 表和主题重拍

## 以后

- 语音提示
- 写校准（现在只读）
- 「安全开机」「星（手动 / 自动）」这两项设置真正起作用
- 在空中实测：热点的 CW ID、卫星过境、脱网、按距离排序、AM / SSB 静噪延时

**P25**（详见 [p25.md](p25.md) 第 6 节）

- 开机后第一次通话收不到 HDU
- 个呼
- 弱信号灵敏度测量，和商用 P25 电台、MMDVM 热点互通实测
- 中继的上下行处理

**NXDN**（NXDN48 收发已完成，见 [nxdn/README.md](../nxdn/README.md)）

- NXDN ID 到呼号的数据库（来电显示呼号）
- 解调改进（空中语音帧误码约 1.4 %：均衡）
- 和商用 NXDN 电台互通实测

## 考虑中（需要较多时间和测试设备）

- NXDN96、NXDN 集群和加密
- P25 加密（DES / AES）、集群、Phase 2（TDMA）
