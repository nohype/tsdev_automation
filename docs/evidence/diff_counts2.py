import re

# master-record progIds from the 18:47 bulk dump
bulk = {}
cur = None
for line in open(r"C:\Users\freek\projects\tsdev_automation\tmp\tvs1_bulk_dump_out2.txt",
                 encoding="utf-8", errors="replace"):
    m = re.match(r"^===== (.*?) \(cat (\d+)\) =====", line)
    if m:
        cur = (m.group(1), int(m.group(2))); bulk[cur] = []; continue
    m = re.match(r"^  \[rec (\d+)\] progId=(\d+)", line)
    if m and cur:
        bulk[cur].append((int(m.group(1)), int(m.group(2))))

com = {}
cur = None
for line in open(r"C:\Users\freek\projects\tsdev_automation\tmp\enum_run_base.txt", encoding="utf-16"):
    m = re.match(r"^=== typeVal=(-?\d+)", line)
    if m:
        cur = int(m.group(1)); com.setdefault(cur, []); continue
    m = re.match(r"^ROW\t(\d+)\t(.*)$", line.rstrip("\n"))
    if m and cur is not None:
        com[cur].append((int(m.group(1)), m.group(2)))

for key in sorted(bulk, key=lambda k: k[1]):
    rows = bulk[key]
    ids = [i for _, i in rows]
    print(f"bulk {key}: rows={len(ids)} uniq={len(set(ids))}")

s0 = set(i for _, i in bulk[("strategy", 0)])
c0 = set(i for i, _ in com[0])
print("strategy: only_bulk=%s only_com=%s" % (sorted(s0 - c0), sorted(c0 - s0)))

s1 = set(i for _, i in bulk[("function", 1)])
c1 = set(i for i, _ in com[1])
print("function: only_bulk=%s only_com=%s" % (sorted(s1 - c1), sorted(c1 - s1)))
dups1 = sorted(i for i in s1 if [j for _, j in bulk[("function", 1)]].count(i) > 1)
print("function bulk dup progIds:", dups1)

s2 = set(i for _, i in bulk[("indicator family", 2)])
c2 = set(i for i, _ in com[2]) | set(i for i, _ in com[1282]) | set(i for i, _ in com[5122])
print("cat2 family: bulk=%d com_union=%d" % (len(s2), len(c2)))
print("  only_bulk=%s" % sorted(s2 - c2))
print("  only_com=%s" % sorted(c2 - s2))
print("  bulk cat2 rows in id order:", sorted(s2)[:20], "...")
# id ranges of the only_bulk extras
extra = sorted(s2 - c2)
print("  extras:", extra)

# name map from com for context
names = {}
for tv in (0, 1, 2, 1282, 5122):
    for i, n in com[tv]:
        names[(tv, i)] = n