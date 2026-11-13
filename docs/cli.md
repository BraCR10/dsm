# dsm_cli

`dsm_cli` is a client of the DSM API, like any application. It attaches to a node running on the same
machine and asks it for information about the cluster.

```sh
./build/dsm_cli [--config <path>] [--node <id>] <command>
```

| Option | Meaning |
| --- | --- |
| `--config <path>` | Cluster configuration, `config/cluster.json` by default |
| `--node <id>` | Local node to attach to; by default the first node running on this machine |

If no node is running, every command prints `No DSM node is running on this machine. Start one with
dsm_node.` and exits with code 1.

A client can only attach to a node of its own machine, because it uses the node's Unix socket and shared
memory. If `--node` names a node that is not in the configuration or whose IP belongs to another machine,
the command prints `The requested node is not in the configuration or does not run on this machine.` and
exits with code 1. Information about remote nodes is requested through the local node, for example
`dsm_cli pages 2`.

## Commands

### status

Shows the node table of the cluster, as seen by the attached node.

```sh
./build/dsm_cli status
```

```
NODE   IP               MAIN   HEARTBEAT  STATE    MEMORY       FREE
0      127.0.0.1        8080   8280       online   1048576      1044480
1      127.0.0.1        8081   8281       online   4194304      4194304
2      127.0.0.1        8082   8282       offline  2097152      2097152
```

### stats

Shows the statistics of the attached node.

```sh
./build/dsm_cli --node 1 stats
```

```
Page faults        1
Pages sent         0
Pages received     1
Invalidations      0
Messages sent      8
Messages received  8
Used memory        0 bytes
Free memory        4194304 bytes
```

### pages

Shows the page table of any node. The attached node asks the other node when needed.

```sh
./build/dsm_cli --node 1 pages 0
```

```
Node 0 page table (1 pages)
PAGE       HOME   STORE  HOLDER   STATE  PROCESS  ORDER  COPIES
0          0      0      0        S      91417    0      1
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
| `1` | No node running, invalid configuration or query failed |
| `2` | Invalid arguments |
