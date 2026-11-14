# Windows PowerShell 运行指南：SignalFlight Systems V1.8

本工程无需Linux、WSL、Visual Studio、AI、Agent、CUDA、ONNX或OpenCV。需要Windows 10/11、MSYS2 UCRT64 GCC、PowerShell以及Python 3（只使用标准库）。

## 首次验收

```powershell
cd 'E:\c++\项目\SignalFlight'
$env:Path = 'E:\msys64\ucrt64\bin;' + $env:Path
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\run_windows.ps1 -Mode test
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\run_windows.ps1 -Mode benchmark -SkipBuild
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\run_windows.ps1 -Mode fault -SkipBuild
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\run_windows.ps1 -Mode overload -SkipBuild
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\run_windows.ps1 -Mode qualify -SkipBuild
```

`build_windows.ps1`在构建前保留系统默认ANSI代码页，规避MSYS2 `as.exe`将中文路径输出文件误转码的问题；构建后再切换UTF-8显示程序输出。这是此前Windows电脑实测发现并修复过的兼容问题。不要在编译前手动运行`chcp 65001`。

## 如果报错

- 找不到 `g++.exe`：检查`E:\msys64\ucrt64\bin\g++.exe`或设置`$env:SF_CXX`。
- 找不到`build_windows.ps1`：确认解压的是完整发行包，脚本应在`scripts/`内。
- Windows文件被占用：项目在并发关闭测试中明确要求所有文件流退出作用域后才能删除目录；仍出现时保留日志用于排查。
- 命令提示`python.exe`来自MSYS2：没关系，默认Python脚本只用标准库，不依赖pip和cv2；如运行库缺失可安装正常Python 3。
- `cannot convert character sequence`：当前V1.8保留Win32宽字符命令行与UTF-16路径转换；发送实际终端输出协助定位。
- 要观察全部环境：`g++ --version; python --version; Test-Path .\CMakeLists.txt`。

## 版本与证据

`Select-String .\CMakeLists.txt -Pattern 'project\(SignalFlight VERSION'`必须显示`1.8.0`。旧V1.6实测性能不能直接当作V1.8的成绩。验收输出包括 `benchmarks/verification_report.json`、`benchmarks/qualification_report.json`、`benchmarks/dsp_comparison.json`和`benchmarks/crc_comparison.json`。
