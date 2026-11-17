#!/usr/bin/env python3
"""Checks that every T("...") text in Eco's code has a Spanish translation in common/i18n_es.inc.

   python tools/check_translations.py        (exit code 1 if something is missing)

Also checks the command arguments and descriptions in client/commands.hpp."""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOURCES = ['client', 'server', 'common', 'tools']


def literals(src):
    """(start, end) of every plain "..." literal in code (no comments, no preprocessor lines, no L/u8/R prefixes)."""
    i, n, out, line_start = 0, len(src), [], True
    while i < n:
        c = src[i]
        if line_start and c in ' \t':
            i += 1
            continue
        if line_start and c == '#':
            while i < n and src[i] != '\n':
                i += 2 if src[i] == '\\' and i + 1 < n and src[i + 1] == '\n' else 1
            continue
        line_start = False
        if c == '\n':
            line_start = True
            i += 1
        elif src.startswith('//', i):
            j = src.find('\n', i)
            i = n if j < 0 else j
        elif src.startswith('/*', i):
            j = src.find('*/', i + 2)
            i = n if j < 0 else j + 2
        elif c == "'":
            j = i + 1
            while j < n and src[j] != "'":
                j += 2 if src[j] == '\\' else 1
            i = j + 1
        elif c == '"':
            prefix = re.search(r'([A-Za-z0-9_]*)$', src[max(0, i - 3):i]).group(1)
            if prefix.endswith('R'):
                m = re.match(r'"([^(]*)\(', src[i:])
                i = src.find(')' + m.group(1) + '"', i) + len(m.group(1)) + 2
                continue
            j = i + 1
            while src[j] != '"':
                j += 2 if src[j] == '\\' else 1
            if prefix == '':
                out.append((i, j + 1))
            i = j + 1
        elif c.isalnum() or c == '_':
            while i < n and (src[i].isalnum() or src[i] == '_'):
                i += 1
        else:
            i += 1
    return out


def joined(src, lits):
    """Adjacent literals ("a" "b") count as one."""
    g = []
    for s, e in lits:
        if g and src[g[-1][1]:s].strip() == '':
            g[-1] = (g[-1][0], e)
        else:
            g.append((s, e))
    return g


def content(text):
    return ''.join(re.findall(r'"((?:[^"\\]|\\.)*)"', text, re.S))


def used_keys():
    keys = {}
    for d in SOURCES:
        for dirpath, _, files in os.walk(os.path.join(ROOT, d)):
            for f in files:
                if not f.endswith(('.cpp', '.hpp')):
                    continue
                path = os.path.join(dirpath, f)
                src = open(path, encoding='utf-8').read()
                for s, e in joined(src, literals(src)):
                    if src[max(0, s - 2):s] == 'T(' and src[e:e + 1] == ')':
                        k = content(src[s:e])
                        keys.setdefault(k, f'{os.path.relpath(path, ROOT)}:{src.count(chr(10), 0, s) + 1}')
    cmds = open(os.path.join(ROOT, 'client', 'commands.hpp'), encoding='utf-8').read()
    for m in re.finditer(r'\{"[a-z0-9]+", "[a-z0-9]+", "((?:[^"\\]|\\.)*)", "((?:[^"\\]|\\.)*)"', cmds):
        for k in m.groups():
            if re.search('[a-záéíóúñ]', k):
                keys.setdefault(k, 'client/commands.hpp')
    return keys


def table():
    src = open(os.path.join(ROOT, 'common', 'i18n_es.inc'), encoding='utf-8').read()
    x = {}
    for m in re.finditer(r'^X\("((?:[^"\\]|\\.)*)", "((?:[^"\\]|\\.)*)"\)', src, re.M):
        x[m.group(1)] = m.group(2)
    return x


def main():
    keys, x = used_keys(), table()
    missing = {k: where for k, where in keys.items() if k not in x and k.split('##')[0] not in x}
    for k, where in sorted(missing.items(), key=lambda kv: kv[1]):
        print(f'missing  {where}: "{k[:100]}"')
    print(f'{len(keys)} texts, {len(missing)} without Spanish translation')
    return 1 if missing else 0


if __name__ == '__main__':
    sys.exit(main())
