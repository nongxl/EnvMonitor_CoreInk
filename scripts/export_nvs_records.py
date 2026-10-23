#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
CoreInk Humidor & Environmental Monitor - NVS 历史数据导出工具

功能：
1. 自动调用 esptool 经由 USB 串口从 ESP32 的 NVS 分区 (0x9000 ~ 0xE000, 20KB) 读取原始固件存储；
2. 自动解析 ESP-IDF NVS 二进制数据结构（包含命名空间查找、单/多页 BLOB 拼包与版本序列匹配）；
3. 将存储的每小时 EnvRecord 结构体（40 字节）解码为可读的数值与时间；
4. 导出为 CSV / JSON 表格文件，并在终端打印统计摘要。
"""

import os
import sys
import argparse
import struct
import datetime
import subprocess
import shutil

# 确保在 Windows 控制台下输出中文不乱码
if sys.platform.startswith('win'):
    try:
        sys.stdout.reconfigure(encoding='utf-8', errors='replace')
        sys.stderr.reconfigure(encoding='utf-8', errors='replace')
    except Exception:
        pass

# EnvRecord 结构定义 (40 字节，小端对齐: uint32 + 8个float + 3个uint8 + 1字节padding)
# struct EnvRecord {
#     uint32_t timestamp;
#     float temp, humi, press, iaq, staticIaq, eco2, bvoc, gasRes;
#     uint8_t iaqAccuracy, stabStatus, runInStatus;
# };
RECORD_FORMAT = '<I8f3Bx'
RECORD_SIZE = struct.calcsize(RECORD_FORMAT) # 40 字节

NVS_OFFSET = 0x9000
NVS_SIZE = 0x5000 # 20480 字节 (5 个 4KB 页)
PAGE_SIZE = 4096


def find_esptool():
    """查找可用的 esptool 可执行文件路径"""
    # 1. 尝试当前 Python 环境同级目录下的 esptool
    py_dir = os.path.dirname(sys.executable)
    for candidate in [
        os.path.join(py_dir, 'esptool.exe'),
        os.path.join(py_dir, 'esptool'),
        os.path.join(py_dir, 'Scripts', 'esptool.exe'),
    ]:
        if os.path.isfile(candidate):
            return candidate

    # 2. 尝试 PlatformIO 默认虚拟环境路径
    user_home = os.path.expanduser('~')
    pio_esptool = os.path.join(user_home, '.platformio', 'penv', 'Scripts', 'esptool.exe')
    if os.path.isfile(pio_esptool):
        return pio_esptool

    # 3. 环境变量中的 esptool
    sys_esptool = shutil.which('esptool.py') or shutil.which('esptool')
    if sys_esptool:
        return sys_esptool

    return None


def read_nvs_from_device(port, baud, output_bin):
    """通过 esptool 从物理设备读取 NVS 分区二进制"""
    esptool_bin = find_esptool()
    if not esptool_bin:
        raise RuntimeError(
            "未找到 esptool 工具！请确保已安装 PlatformIO 或运行 'pip install esptool'。"
        )

    cmd = [
        esptool_bin,
        '--port', port,
        '--baud', str(baud),
        'read_flash',
        hex(NVS_OFFSET),
        hex(NVS_SIZE),
        output_bin
    ]
    print(f"[1/3] 正在从设备 ({port} @ {baud} bps) 读取 NVS 闪存数据...")
    print(f"      执行命令: {' '.join(cmd)}")
    
    res = subprocess.run(cmd, capture_output=True, text=True)
    if res.returncode != 0:
        err_msg = res.stderr or res.stdout
        if "PermissionError" in err_msg or "拒绝访问" in err_msg:
            raise RuntimeError(f"串口 {port} 打开失败！可能当前串口监视器 (Serial Monitor) 正在运行占用端口，请先关闭串口监视器。")
        raise RuntimeError(f"读取闪存失败:\n{err_msg}")
    
    print("[1/3] NVS 分区读取成功 (20,480 字节)！")


def parse_nvs_data(raw_data):
    """
    解析 ESP-IDF NVS 分区数据并提取 env_data:records 的二进制数据
    """
    num_pages = len(raw_data) // PAGE_SIZE
    if num_pages == 0:
        raise ValueError("NVS 数据长度不足一个页面 (4KB)")

    namespaces = {0: 'nvs'}
    
    # 扫描页面，按序列号管理
    pages = []
    for p in range(num_pages):
        page = raw_data[p * PAGE_SIZE : (p + 1) * PAGE_SIZE]
        state, seq = struct.unpack('<II', page[0:8])
        if state == 0xFFFFFFFF: # 未写入/未激活的空页
            continue
        pages.append((p, seq, page))

    if not pages:
        raise ValueError("NVS 分区未发现任何已写入的活跃页面")

    # 按 seq 排序（从旧到新）
    pages.sort(key=lambda x: x[1])

    # 1. 查找命名空间 ID (key='env_data')
    target_ns_id = None
    for p_idx, seq, page in pages:
        bitmap = page[32:64]
        entry_idx = 2
        offset = 64
        while entry_idx < 128:
            byte_pos = entry_idx // 4
            bit_pos = (entry_idx % 4) * 2
            entry_state = (bitmap[byte_pos] >> bit_pos) & 0x03
            
            if entry_state == 0x02: # 写入状态
                ns, dtype, span, chunk_idx, crc = struct.unpack('<BBBBI', page[offset:offset+8])
                key = page[offset+8:offset+24].split(b'\x00')[0].decode('ascii', errors='ignore')
                data_field = page[offset+24:offset+32]
                
                if ns == 0 and dtype == 0x01: # 命名空间登记项
                    ns_id = data_field[0]
                    namespaces[ns_id] = key
                    if key == 'env_data':
                        target_ns_id = ns_id
                
                span = max(1, span)
                entry_idx += span
                offset += span * 32
            else:
                entry_idx += 1
                offset += 32

    if target_ns_id is None:
        raise ValueError("在 NVS 中未找到命名空间 'env_data'，可能设备尚未执行过历史数据存储")

    print(f"      找到环境数据命名空间: 'env_data' (ID = {target_ns_id})")

    # 2. 收集 target_ns_id 下的所有有效记录项
    # dtype 说明: 0x42 为单页 BLOB 或多页 BLOB Chunk，0x48 为多页 BLOB Index
    blob_index = None
    blob_chunks = {} # chunk_idx -> bytes
    single_blobs = [] # list of (seq, entry_idx, bytes)
    rec_count = 0

    for p_idx, seq, page in pages:
        bitmap = page[32:64]
        entry_idx = 2
        offset = 64
        while entry_idx < 128:
            byte_pos = entry_idx // 4
            bit_pos = (entry_idx % 4) * 2
            entry_state = (bitmap[byte_pos] >> bit_pos) & 0x03
            
            if entry_state == 0x02: # Written
                ns, dtype, span, chunk_idx, crc = struct.unpack('<BBBBI', page[offset:offset+8])
                key = page[offset+8:offset+24].split(b'\x00')[0].decode('ascii', errors='ignore')
                data_field = page[offset+24:offset+32]
                
                if ns == target_ns_id:
                    if key == 'count' and dtype in (0x01, 0x02, 0x04):
                        rec_count = struct.unpack('<H', data_field[:2])[0]
                    elif key == 'records':
                        if dtype == 0x48: # BLOB INDEX
                            total_size, chunk_count, chunk_start = struct.unpack('<IBB', data_field[:6])
                            blob_index = (total_size, chunk_count, chunk_start, seq)
                        elif dtype == 0x42: # BLOB DATA
                            blob_size, = struct.unpack('<H', data_field[:2])
                            payload = page[offset+32 : offset + 32 + blob_size]
                            if chunk_idx != 255: # 多页分块
                                blob_chunks[chunk_idx] = payload
                            else: # 单页 BLOB
                                single_blobs.append((seq, entry_idx, payload))

                span = max(1, span)
                entry_idx += span
                offset += span * 32
            else:
                entry_idx += 1
                offset += 32

    # 组装最终的数据二进制流
    final_blob = b''
    if blob_index and blob_chunks:
        total_size, chunk_count, chunk_start, _ = blob_index
        print(f"      检测到多页连续 BLOB: 总大小 {total_size} 字节，分块数 {chunk_count}")
        assembled = b''
        for c in range(chunk_start, chunk_start + chunk_count):
            if c in blob_chunks:
                assembled += blob_chunks[c]
            else:
                print(f"      [警告] 缺少分块 Chunk #{c}")
        final_blob = assembled[:total_size]
    elif single_blobs:
        # 取最新的一份单页 BLOB
        single_blobs.sort(key=lambda x: (x[0], x[1]))
        final_blob = single_blobs[-1][2]
        print(f"      检测到单页 BLOB: 大小 {len(final_blob)} 字节")
    else:
        raise ValueError("在 NVS 中未找到有效的 'records' 数据项")

    # 解码 EnvRecord 列表
    total_records = len(final_blob) // RECORD_SIZE
    records = []
    for i in range(total_records):
        chunk = final_blob[i * RECORD_SIZE : (i + 1) * RECORD_SIZE]
        ts, temp, humi, press, iaq, static_iaq, eco2, bvoc, gas_res, acc, stab, runin = struct.unpack(RECORD_FORMAT, chunk)
        
        # 转换时间戳为可读时间
        try:
            dt = datetime.datetime.fromtimestamp(ts)
            dt_str = dt.strftime('%Y-%m-%d %H:%M:%S')
        except Exception:
            dt_str = "无效时间戳"

        records.append({
            'index': i + 1,
            'timestamp': ts,
            'datetime': dt_str,
            'temperature': round(temp, 2),
            'humidity': round(humi, 2),
            'pressure': round(press, 2),
            'iaq': round(iaq, 1),
            'static_iaq': round(static_iaq, 1),
            'eco2': round(eco2, 1),
            'bvoc': round(bvoc, 3),
            'gas_resistance': round(gas_res, 2),
            'iaq_accuracy': acc,
            'heater_stable': bool(stab),
            'runin_complete': bool(runin)
        })

    return records, rec_count


def export_to_csv(records, filepath):
    """将环境数据导出为 CSV 文件"""
    headers = [
        "序号", "Unix时间戳", "记录时间", "温度(°C)", "湿度(%RH)",
        "气压(hPa)", "动态IAQ", "静态IAQ", "eCO2(ppm)", "bVOC(ppm)",
        "气敏阻抗(kΩ)", "校准精度等级", "加热稳定", "预热完成"
    ]
    with open(filepath, 'w', encoding='utf-8-sig') as f:
        f.write(','.join(headers) + '\n')
        for r in records:
            row = [
                str(r['index']),
                str(r['timestamp']),
                r['datetime'],
                f"{r['temperature']:.2f}",
                f"{r['humidity']:.2f}",
                f"{r['pressure']:.2f}",
                f"{r['iaq']:.1f}",
                f"{r['static_iaq']:.1f}",
                f"{r['eco2']:.1f}",
                f"{r['bvoc']:.3f}",
                f"{r['gas_resistance']:.2f}",
                str(r['iaq_accuracy']),
                "是" if r['heater_stable'] else "否",
                "是" if r['runin_complete'] else "否"
            ]
            f.write(','.join(row) + '\n')


def export_to_json(records, filepath):
    """将环境数据导出为 JSON 文件"""
    import json
    with open(filepath, 'w', encoding='utf-8') as f:
        json.dump(records, f, indent=2, ensure_ascii=False)


def print_summary(records):
    """打印环境数据摘要与终端表格"""
    if not records:
        print("[!] 记录为空")
        return

    print("\n" + "=" * 90)
    print(f"  CoreInk NVS 历史环境记录摘要 (共 {len(records)} 条整点快照)")
    print("=" * 90)
    print(f"  时间跨度 : {records[0]['datetime']}  ~  {records[-1]['datetime']}")
    
    temps = [r['temperature'] for r in records]
    humis = [r['humidity'] for r in records]
    iaqs = [r['iaq'] for r in records]
    
    print(f"  温度统计 : 最低 {min(temps):.1f}°C | 最高 {max(temps):.1f}°C | 平均 {sum(temps)/len(temps):.1f}°C")
    print(f"  湿度统计 : 最低 {min(humis):.1f}% | 最高 {max(humis):.1f}% | 平均 {sum(humis)/len(humis):.1f}%")
    print(f"  空气质量 : 动态 IAQ 均值 {sum(iaqs)/len(iaqs):.0f} | 最佳 {min(iaqs):.0f} | 最差 {max(iaqs):.0f}")
    print("-" * 90)
    print(f"{'序号':^4} | {'记录时间':^19} | {'温度':^7} | {'湿度':^7} | {'气压':^8} | {'IAQ':^5} | {'eCO2':^7} | {'精度'}")
    print("-" * 90)
    for r in records:
        print(f"{r['index']:^4} | {r['datetime']:^19} | {r['temperature']:>5.1f}°C | {r['humidity']:>5.1f}% | {r['pressure']:>6.1f}hPa | {r['iaq']:>4.0f} | {r['eco2']:>5.0f}ppm | Lv {r['iaq_accuracy']}")
    print("=" * 90 + "\n")


def main():
    parser = argparse.ArgumentParser(
        description="CoreInk Humidor & Environmental Monitor - NVS 历史数据导出工具",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""示例用法:
  python scripts/export_nvs_records.py                     # 自动从 COM11 读取并导出为 CSV
  python scripts/export_nvs_records.py --port COM5         # 指定串口
  python scripts/export_nvs_records.py --file dump.bin     # 解析离线 NVS 镜像
  python scripts/export_nvs_records.py -o my_data.csv      # 自定义输出文件
  python scripts/export_nvs_records.py --json my_data.json # 同时导出 JSON 格式
"""
    )
    parser.add_argument('--port', '-p', default='COM11', help='设备串口名称 (默认: COM11)')
    parser.add_argument('--baud', '-b', type=int, default=921600, help='读取固件波特率 (默认: 921600)')
    parser.add_argument('--file', '-f', default=None, help='离线 NVS 镜像文件路径 (指定后将直接解析文件，不连接设备)')
    parser.add_argument('--output', '-o', default=None, help='输出 CSV 文件路径 (默认: env_records_YYYYMMDD_HHMMSS.csv)')
    parser.add_argument('--json', default=None, help='额外输出 JSON 文件路径 (可选)')
    parser.add_argument('--keep-bin', action='store_true', help='保留导出的原始 NVS 二进制文件')

    args = parser.parse_args()

    # 确定输出 CSV 默认文件名
    now_str = datetime.datetime.now().strftime('%Y%m%d_%H%M%S')
    csv_path = args.output or f"env_records_{now_str}.csv"
    bin_path = args.file or f"temp_nvs_{now_str}.bin"

    try:
        # 1. 采集或载入 NVS 镜像
        if args.file:
            print(f"[1/3] 正在载入离线 NVS 镜像文件: {args.file} ...")
            if not os.path.isfile(args.file):
                raise FileNotFoundError(f"文件不存在: {args.file}")
            with open(args.file, 'rb') as f:
                raw_data = f.read()
        else:
            read_nvs_from_device(args.port, args.baud, bin_path)
            with open(bin_path, 'rb') as f:
                raw_data = f.read()

        # 2. 解析数据
        print("[2/3] 正在解析 NVS 分区与 EnvRecord 数据结构...")
        records, rec_count = parse_nvs_data(raw_data)
        print(f"[2/3] 成功解码出 {len(records)} 条有效历史整点快照 (NVS 记录计数: {rec_count})！")

        # 3. 导出与展示
        print(f"[3/3] 正在生成导出文件...")
        export_to_csv(records, csv_path)
        print(f"      -> CSV 文件已保存: {os.path.abspath(csv_path)}")

        if args.json:
            export_to_json(records, args.json)
            print(f"      -> JSON 文件已保存: {os.path.abspath(args.json)}")

        # 打印摘要
        print_summary(records)

    except Exception as e:
        print(f"\n[错误] {e}", file=sys.stderr)
        sys.exit(1)
    finally:
        # 若为在线读取且未要求保留临时 bin，则自动清理
        if not args.file and not args.keep_bin and os.path.isfile(bin_path):
            try:
                os.remove(bin_path)
            except Exception:
                pass


if __name__ == '__main__':
    main()
