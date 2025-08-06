from collections import Counter

def extract_mnemonic(line):
    """
    提取一行中的助记符（指令名），例如 'vse64.v'。
    忽略空行和非指令行。
    """
    line = line.strip()
    if not line:
        return None
    # 取第一个“词”，作为指令名
    return line.split()[0]

def count_vector_instructions(filename):
    counter = Counter()

    with open(filename, 'r') as f:
        for line in f:
            mnemonic = extract_mnemonic(line)
            if mnemonic:
                counter[mnemonic] += 1

    return counter

def main():
    log_file = "plugin_vinstrace.txt"  # 修改为你的日志文件名
    instr_count = count_vector_instructions(log_file)

    print("Vector instruction frequency:")
    for instr, count in instr_count.most_common():
        print(f"{instr:<15} {count}")

if __name__ == "__main__":
    main()
