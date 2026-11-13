# dsm_cli

`dsm_cli` queries running DSM nodes through their `main` port. It does not join the cluster; it sends
`STATS_REQUEST` or `PAGE_TABLE_REQUEST` and prints the answer.

```sh
./build/dsm_cli --config <path> [--node <id>] <command>
```

| Option | Meaning |
| --- | --- |
| `--config <path>` | Cluster configuration, the same file the nodes use |
| `--node <id>` | Node to query, required by `stats` and `pages` |

## Commands

### status

Shows every node in the configuration, online or offline.

```sh
./build/dsm_cli --config config/cluster.json status
```

```
Node 0  online  used 4096 bytes  free 1044480 bytes  faults 0
Node 1  online  used 0 bytes  free 4194304 bytes  faults 1
Node 2  offline
```

### stats

Shows the statistics of one node.

```sh
./build/dsm_cli --config config/cluster.json --node 1 stats
```

```
Node 1 statistics
  Page faults        1
  Pages sent         0
  Pages received     1
  Invalidations      0
  Messages sent      7
  Messages received  7
  Used memory        0 bytes
  Free memory        4194304 bytes
```

### pages

Shows the page table of one node: every page it allocated as home or holds as a copy.

```sh
./build/dsm_cli --config config/cluster.json --node 0 pages
```

```
Node 0 page table (1 pages)
  PAGE       HOME   STORE  HOLDER   STATE  PROCESS  ORDER  COPIES
  0          0      0      0        S      77878    0      1
```

| Column | Meaning |
| --- | --- |
| `PAGE` | Global page id |
| `HOME` | Node that owns the address |
| `STORE` | Node that stores the data |
| `HOLDER` | Node that holds the current version |
| `STATE` | MSI state in the queried node (`M`, `S` or `I`) |
| `PROCESS` | Process that allocated the page, on the home node |
| `ORDER` | Position of the page inside its block |
| `COPIES` | Nodes with a shared copy, on the home node |

## Exit codes

| Code | Meaning |
| --- | --- |
| `0` | Query answered |
| `1` | Invalid configuration, unknown node or node not reachable |
| `2` | Invalid arguments |
