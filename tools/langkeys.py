#!/usr/bin/env python3
"""Lists every piece of English menu text in src/*.cpp -> include/LangKeys.inc (the translation template's lines).
Run from the project folder after changing menu text:  python3 tools/langkeys.py"""
import glob, os, re, sys

CALLS = {"Checkbox","Button","SmallButton","SliderFloat","SliderInt","ColorEdit3","TreeNode","CollapsingHeader","InputText",
         "Selectable","BeginCombo","Combo","Begin","SeparatorText","ProgressBar","TextUnformatted","Text","TextWrapped",
         "TextDisabled","TextColored","BulletText","SetTooltip"}
HELPERS = {"Help","HelpMarker","Section","ConfirmButton","ComboValue","Pct","Mins","Minutes","Bar","ColourRow","PageName"}

def tokens(src):
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if src.startswith("//", i): i = src.find("\n", i); i = n if i < 0 else i; continue
        if src.startswith("/*", i): i = src.find("*/", i) + 2; continue
        if c == '"':
            j = i + 1; buf = []
            while src[j] != '"':
                if src[j] == "\\":
                    e = src[j+1]; buf.append({"n": "\n", "t": "\t", '"': '"', "\\": "\\", "'": "'", "0": "\0"}.get(e, "\\" + e)); j += 2
                else: buf.append(src[j]); j += 1
            yield ("str", "".join(buf), i); i = j + 1; continue
        if c == "'":
            j = i + 1
            while src[j] != "'": j += 2 if src[j] == "\\" else 1
            yield ("chr", src[i:j+1], i); i = j + 1; continue
        if c.isalnum() or c == "_":
            j = i
            while j < n and (src[j].isalnum() or src[j] == "_"): j += 1
            yield ("id", src[i:j], i); i = j; continue
        if src.startswith("::", i): yield ("op", "::", i); i += 2; continue
        if not c.isspace(): yield ("op", c, i)
        i += 1

def runs(toks):
    """adjacent string literals joined; a run stays inside one call depth"""
    out, cur = [], None
    for t in toks:
        if t[0] == "str": cur = (cur or "") + t[1]
        else:
            if cur is not None: out.append(cur); cur = None
    if cur is not None: out.append(cur)
    return out

def keys_of(path):
    toks = list(tokens(open(path, encoding="utf-8", errors="replace").read()))
    found = []
    i = 0
    while i < len(toks):
        t = toks[i]
        name = None
        if t[0] == "id" and t[1] == "Tr" and i + 3 < len(toks) and toks[i+1][1] == "::" and toks[i+2][1] in CALLS and toks[i+3][1] == "(":
            name, start = toks[i+2][1], i + 4
        elif t[0] == "id" and t[1] in HELPERS and i + 1 < len(toks) and toks[i+1][1] == "(" and not (i and toks[i-1][1] in ("::", ".")) \
                and not (i and toks[i-1][0] == "id" and toks[i-1][1] in ("bool", "void", "int", "char")):
            name, start = t[1], i + 2
        elif t[0] == "id" and t[1] == "Lang" and i + 3 < len(toks) and toks[i+1][1] == "::" and toks[i+2][1] in ("T", "L", "F", "TS") and toks[i+3][1] == "(":
            name, start = toks[i+2][1], i + 4
        if name == "TreeNode" and start < len(toks) and toks[start][0] == "str" and start + 1 < len(toks) and toks[start+1][1] == ",":
            start += 2   # TreeNode("id", "fmt", ...): the first one is only an ID
        if name:
            depth, j, seg = 1, start, []
            fmt_depth = None
            while j < len(toks) and depth:
                tk = toks[j]
                if tk[1] == "(":
                    depth += 1
                    if j >= 3 and toks[j-1][1] == "format" and toks[j-2][1] == "::" and fmt_depth is None: fmt_depth = depth
                elif tk[1] == ")":
                    if fmt_depth == depth: fmt_depth = None
                    depth -= 1
                if tk[0] == "str":
                    s = tk[1]
                    if fmt_depth is not None:   # std::format(...) inside: only a label part before "##"
                        if "##" in s and "{" not in s.split("##")[0]: seg.append(("lbl", s))
                    else: seg.append(("str", s))
                else: seg.append(("x", None))
                j += 1
            cur = None
            for k, s in seg + [("x", None)]:
                if k == "str": cur = (cur or "") + s
                else:
                    if cur is not None: found.append(cur); cur = None
                    if k == "lbl": found.append(s)
            i = j; continue
        # tables of structs whose strings are menu text: Type NAME[] = {{...}, ...} - which fields (0-based) are text
        TABLES = {"Rec": (2, 3), "GlobalSetting": (2, 6, 7), "JigglePart": (1,)}
        if t[0] == "id" and t[1] in TABLES and i + 4 < len(toks) and toks[i+1][0] == "id" and toks[i+2][1] == "[":
            fields = TABLES[t[1]]
            m = i + 2
            while m < len(toks) and toks[m][1] != "{": m += 1
            depth, m, field, row = 1, m + 1, 0, []
            while m < len(toks) and depth:
                tk = toks[m]
                if tk[1] == "{":
                    depth += 1
                    if depth == 2: field = 0; row = []
                elif tk[1] == "}":
                    depth -= 1
                elif tk[1] == "," and depth == 2: field += 1
                elif tk[0] == "str" and depth == 2 and field in fields:
                    if row and row[-1][0] == field: row[-1] = (field, row[-1][1] + tk[1])
                    else: row.append((field, tk[1]))
                if tk[1] == "}" and depth == 1: found += [x for _, x in row]; row = []
                m += 1
            i = m; continue
        # arrays of menu items: const char* NAME[] = { "...", ... };
        if t[0] == "id" and t[1] == "char" and i + 5 < len(toks):
            j = i + 1
            while j < len(toks) and toks[j][1] in ("*", "const"): j += 1
            if j + 3 < len(toks) and toks[j][0] == "id" and toks[j+1][1] == "[":
                k = j + 2
                while k < len(toks) and toks[k][1] != "]": k += 1
                if k + 2 < len(toks) and toks[k+1][1] == "=" and toks[k+2][1] == "{":
                    m, body = k + 3, []
                    while m < len(toks) and toks[m][1] != "}": body.append(toks[m]); m += 1
                    found += runs(body); i = m; continue
        i += 1
    return found

def clean(s):
    if "##" in s: s = s.split("##")[0]
    if not s.strip(): return None
    if not re.search(r"[A-Za-z]{2}", s): return None        # "%s", "(?)", "x1.00" ...
    if s == "pct": return None
    if s.lower().endswith((".dds", ".ini", ".esp", ".esm", ".nif")): return None
    return s

def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    seen, order = set(), []
    for p in glob.glob(os.path.join(root, "src", "*.cpp")):   # Title("SkyrimNet", "sn", ...): "sn" is only an ID
        for m in re.finditer(r'Title\("[^"]*",\s*"([^"]*)"', open(p, encoding="utf-8", errors="replace").read()): seen.add(m.group(1))
    for p in sorted(glob.glob(os.path.join(root, "src", "*.cpp"))):
        for s in keys_of(p):
            s = clean(s)
            if s and s not in seen: seen.add(s); order.append(s)
    def lit(s):
        return '"' + s.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n").replace("\t", "\\t") + '"'
    with open(os.path.join(root, "include", "LangKeys.inc"), "w", encoding="utf-8") as o:
        o.write("// made by tools/langkeys.py from the menu text in src/*.cpp - run it again after changing menu text\n")
        o.write("const char* const kKeys[] = {\n")
        for s in order: o.write("    " + lit(s) + ",\n")
        o.write("};\n")
    print(len(order), "lines")
main()
