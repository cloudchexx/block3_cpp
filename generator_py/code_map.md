# Python Generator Code Map

## 入口

### `__init__.py`

导出 `DataGenerator`，包版本为 `1.0.0`。

### `__main__.py`

```text
python -m generator_py --cli -> cli.main()
python -m generator_py       -> gui.main()
```

### `run.py`

便捷分发：`--cli`、`--tui`，默认 GUI。

## 核心：`core.py`

### `DataGenerator`

状态：

- 三维尺寸和值域；
- 输出路径；
- NumPy/纯 Python 引擎；
- `threading.Event` 取消标志。

生成流程：

```text
generate(progress_callback)
 -> validate dimensions/path
 -> create output directory
 -> open output
 -> _write_header()                 # default 48-byte 3DDF header
 -> _write_data_numpy() or _write_data_python()
 -> return completion/cancel status
```

引擎：

- NumPy：`default_rng().uniform()` 分大块生成并转 `float32`。
- Python：X/Y/Z 三重循环，缓冲后用 little-endian `struct.pack` 写入。

两条路径都没有用户可控 seed，默认不可复现。

默认头格式：

```text
<4sIQQQffQ
magic/version/dim_x/dim_y/dim_z/min/max/timestamp
```

该头与 C++ 原始 `.dat` 输入不兼容。

## 前端

### `cli.py`

参数包括尺寸、值域、输出路径、引擎、`--list-engines` 和 `--no-header`。`--no-header` 是当前唯一直接生成 C++ 兼容文件的前端选项。

### `gui.py`

Tkinter UI：配置生成器、估算大小、后台线程生成、进度和取消。始终使用默认带头格式。

### `tui.py`

Textual UI：终端表单、后台 worker、进度和取消。Textual 缺失时提示安装；始终使用默认带头格式。

## 与 C++ 的连接

```text
generator CLI --no-header
 -> raw float32 X-Y-Z .dat
 -> block3d_cli convert
 -> .b3d
 -> block3d_cli verify <b3d> <raw>
```

GUI/TUI 生成物若未增加无头选项，不能直接进入该流程。