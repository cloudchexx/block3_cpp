# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 本目录职责

这是独立的 Python 随机三维数据生成器，包含核心生成逻辑、argparse CLI、Tk GUI 和 Textual TUI。模块关系见 `code_map.md`。

## 运行命令

```bash
python -m generator_py              # GUI
python -m generator_py --cli        # CLI
python generator_py/run.py          # GUI
python generator_py/run.py --cli    # CLI
python generator_py/run.py --tui    # TUI
```

生成可直接交给 C++ 转换器的原始文件：

```bash
python generator_py/run.py --cli \
  -x 100 -y 200 -z 300 \
  --engine numpy --no-header \
  -o test.dat
```

项目没有 Python 测试、formatter/linter 配置或依赖清单。NumPy 可选但大数据生成需要它获得可用性能；Tkinter 用于 GUI；Textual 用于 TUI。

## 关键格式约束

- `DataGenerator.generate()` 默认写 48 字节 `3DDF` 自定义头。
- C++ `convert_raw_to_blocked()` 要求完全无头的 X-Y-Z `float32` 文件。
- 当前只有 CLI 的 `--no-header` 提供兼容输出；GUI/TUI 默认输出不能直接交给 C++ 转换器。
- 纯 Python 通道的循环顺序必须保持 X 外层、Y 中层、Z 内层；NumPy 通道的一维连续输出具有相同线性语义。
- `estimated_file_size` 当前包含默认头；如果引入正式的无头配置，需要同步估算和所有前端显示。

## 前端与线程

- `DataGenerator` 使用 `threading.Event` 协作取消。
- GUI 在后台 `threading.Thread` 中生成，并通过 Tk `after()` 回 UI 线程更新状态。
- TUI 使用 Textual `@work(thread=True)`；不要从 worker 直接进行不安全的 UI 更新。
- CLI 的 `--no-header` 当前通过替换 `_write_header` 实现。若重构为正式参数，要保持旧命令行为兼容。
- 取消或 I/O 失败可能留下部分文件；修改该行为时同步三个前端的完成/失败提示。

修改核心生成格式或顺序后，应生成一个小型 `--no-header` 文件，并通过 C++ `convert` + `verify` 做端到端检查。