
# 北航 OS 多核移植环境配置指南 (WSL2 版)

## 一、 环境架构

* **宿主机**: Windows 11/10
* **开发环境**: WSL2 (Ubuntu 24.04 LTS)
* **目标架构**: MIPS (32-bit, Malta Platform)
* **编辑器**: VS Code (配合 WSL 扩展与 AI 插件)

## 二、 配置步骤回顾

### 1. 开启 WSL2 与 网络镜像模式

在 Windows 用户目录 (`C:\Users\<YourUsername>\`) 下创建 `.wslconfig` 文件，以确保 AI 插件与 Ubuntu 共享网络代理：

```ini
[wsl2]
networkingMode=mirrored
dnsTunneling=true
firewall=true
autoProxy=true

```

*在 PowerShell 中执行 `wsl --shutdown` 重启生效。* （这个要在windows系统下的命令行中执行）

### 2. 安装核心工具链

在 Ubuntu 终端执行以下命令：

```bash
sudo apt update
sudo apt install -y build-essential gcc-mips-linux-gnu \
                    binutils-mips-linux-gnu gdb-multiarch \
                    qemu-system-mips curl git

```

### 3. VS Code 联动

1. 在 Windows 安装 **WSL** 扩展。
2. 在 Ubuntu 终端输入 `code .` 启动 VS Code。
3. **关键设置**：确保 GitHub Copilot / Codeium 等插件在 **"WSL: Ubuntu"** 栏目下已点击 "Install"。

---

## 三、 验证命令手册

| 工具 | 验证命令 | 预期输出示例 |
| --- | --- | --- |
| **MIPS 编译器** | `mips-linux-gnu-gcc -v` | `gcc version 12.x.x` |
| **QEMU 模拟器** | `qemu-system-mips --version` | `QEMU emulator version 8.x.x` |


---

## 四、 后续如何启动 Ubuntu 系统？

配置完成后，你不再需要通过 PowerShell 的复杂命令进入，推荐以下三种最快的方式：

### 方法 1：VS Code 直接启动

1. 打开 Windows 上的 **VS Code**。
2. 点击左下角的 **蓝色“远程窗口”图标**（两个尖括号 `< >`）。
3. 选择 **"Connect to WSL"**（连接到默认 Ubuntu）或 **"Open Folder in WSL"**（直接打开代码目录）。
4. VS Code 会自动唤醒后台的 Ubuntu 系统。

### 方法 2：Windows 终端（命令行启动）

1. 按下 `Win + R`，输入 `cmd` 或 `powershell`。
2. 输入 `wsl` 即可直接进入你设置好的 Ubuntu。
* *注：如果你有多个 Linux 系统，输入 `wsl -d Ubuntu` 指定进入。*



### 方法 3：开始菜单（快捷方式）

1. 按下 `Win` 键。
2. 搜索 **"Ubuntu"**，点击打开。它会像普通软件一样弹出一个 Linux 终端窗口。

---

## 💡 

* **文件路径**：务必将代码存放在 WSL 的家目录下（如 `~/mos/`），**不要**放在 `/mnt/d/` 下开发，否则多核编译时文件 IO 会慢到让你怀疑人生。
* **退出 QEMU**：在使用 `-nographic` 模式调试时，记住退出快捷键是 `Ctrl + A` 紧接着按 `X`。

