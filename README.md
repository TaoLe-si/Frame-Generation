# Frame Generation

为 Minecraft 客户端接入帧生成与超分。

| | 帧生成 | 超分 |
|---|---|---|
| **NVIDIA** | DLSS-G（含多帧生成解锁） | DLSS-SR |
| **AMD** | FSR3 | FSR |
| **Intel** | XeSS-FG | XeSS-SR |

不改动光影模组本身的渲染逻辑，而是把世界渲染阶段的主渲染目标换成低分辨率缓冲，
光影管线整体跟着降分辨率，再放大写回全分辨率主目标；手持物品与 HUD 保持原生。

## 分支

| 分支 | Minecraft | 加载器 | 光影模组 |
|---|---|---|---|
| `1.21.1-neoforge`（默认） | 1.21.1 | NeoForge 21.1.228 | Sodium + Iris |
| `1.20.1-forge` | 1.20.1 | Forge 47.4.16 | Embeddium + Oculus |

## 结构

```
src/main/java/com/taolesi/dlssmc/
  core/           FGRuntime / DLSSRuntime —— 每帧调度、资源与锁纪律、FPS 采样
  nativebridge/   JNI 桥（DLSSFGNative / DLSSDXNative / DLSSNative）
  render/         深度+运动向量 pass、低分辨率世界目标
  mixin/          世界阶段切目标、抓矩阵、接管上屏
  config/         档位与调试开关
  compat/         光影模组的设置界面接入
native/src/       三个原生后端（C++，直调 Streamline / FidelityFX / XeSS SDK）
```

## 构建

```bash
./gradlew build          # -> build/libs/frame-generation-<version>.jar
```

原生 DLL 是预编译产物，放在 `native/build/`。要重新编译 C++ 需 MSVC：

```bash
bash native/build.sh      # 超分
bash native/build_fg.sh   # 帧生成（Vulkan / DLSS）
bash native/build_dx.sh   # 帧生成（D3D12 / FSR、XeSS）
```

## 依赖

`libs/` 下的光影模组只用于编译（`compileOnly`），不入库；需要时用
`python tools/fetch_mods.py` 取回。厂商二进制（libxess / FidelityFX）按各自许可
随构建产物再分发，源码仓库不包含。

## 运行前提

- 硬件加速 GPU 调度（Windows 图形设置，HAGS）需开启
- DLSS 后端需要本机 <a href="https://github.com/NVIDIAGameWorks/Streamline">Streamline</a> 运行时目录，在配置里指定 `streamlinePath`
- FSR / XeSS 后端不需要额外安装，厂商库随模组打包
