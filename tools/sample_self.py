#!/usr/bin/env python3
"""Tiempo propio por función de un hilo en un informe de `sample` de macOS.

Uso:
    python3 -I tools/sample_self.py <informe.txt> <texto que identifica el hilo> [n] [--callers función]

El hilo se busca por un texto de su pila (p. ej. "GpuThread" o "ThreadMain"); se suman las
muestras de cada función en las que estaba en lo alto de la pila (tiempo propio) dentro del
subárbol de la primera línea que contiene ese texto.
"""
import re
import sys
from collections import Counter

LINE = re.compile(r"^(?P<indent>[\s+!:|]*)(?P<count>\d+) (?P<name>.+?)(?:  \(in [^)]*\))?(?: \+ [\d,.]+)?(?:  \[.*)?$")


def parse(lines):
    nodes = []  # (profundidad, muestras, nombre)
    for line in lines:
        match = LINE.match(line.rstrip("\n"))
        if not match:
            continue
        depth = len(match.group("indent"))
        nodes.append((depth, int(match.group("count")), match.group("name").strip()))
    return nodes


def main():
    args = sys.argv[1:]
    callers_of = None
    if "--callers" in args:
        k = args.index("--callers")
        callers_of = args[k + 1]
        del args[k:k + 2]
    path, needle = args[0], args[1]
    top = int(args[2]) if len(args) > 2 else 25
    with open(path, encoding="utf-8", errors="replace") as f:
        lines = f.readlines()
    nodes = parse(lines)
    start = next(i for i, node in enumerate(nodes) if needle in node[2])
    base_depth = nodes[start][0]
    end = start + 1
    while end < len(nodes) and nodes[end][0] > base_depth:
        end += 1
    subtree = nodes[start:end]

    self_time = Counter()
    for i, (depth, count, name) in enumerate(subtree):
        children = 0
        j = i + 1
        child_depth = None
        while j < len(subtree) and subtree[j][0] > depth:
            if child_depth is None:
                child_depth = subtree[j][0]
            if subtree[j][0] == child_depth:
                children += subtree[j][1]
            j += 1
        self_time[name] += count - children

    if callers_of:
        callers = Counter()
        for i, (depth, count, name) in enumerate(subtree):
            if callers_of in name:
                parent = next((subtree[j][2] for j in range(i - 1, -1, -1) if subtree[j][0] < depth), "?")
                callers[parent] += count
        for name, count in callers.most_common(top):
            print(f"{count:6d}  {name[:110]}")
        return

    total = subtree[0][1]
    print(f"{needle}: {total} muestras")
    for name, count in self_time.most_common(top):
        print(f"{count:6d}  {100.0 * count / total:5.1f}%  {name[:110]}")


if __name__ == "__main__":
    main()
