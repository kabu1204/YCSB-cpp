import os
import sys

def extract(path):
    f = open(path, mode="r")
    lines = f.readlines()
    f.close()
    outs = []
    d = 0
    for i, line in enumerate(lines):
        if d>0:
            outs.append(line)
            d -= 1
            continue
        if line.startswith("** Compaction Stats [default] **") and lines[i+1].startswith("Level"):
            outs.append("\n\n")
            outs.append(line)
            d = 20

    f = open(path+"-out", mode="w")
    f.writelines(outs)
    f.close()
    print("extracted file: {}".format(path+"-out"))


if __name__=="__main__":
    if len(sys.argv)!=2:
        print("Usage: python extract_rocksdb_cdstat.py <path-to-rocksdb-LOG-file>")
        print("\texample: python extract_rocksdb_cdstat.py /tmp/ycsb-rocksdb/LOG")
    logpath = sys.argv[1]
    extract(logpath)