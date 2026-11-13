# dsm_cli

Queries the cluster through the DSM node running on this machine.

```sh
./build/dsm_cli [--node <id>] <command>
```

| Command | Shows |
| --- | --- |
| `status` | Every node of the cluster, online or offline |
| `stats` | Statistics of the local node |
| `pages <id>` | Page table of node `<id>` |

`--node <id>` picks the local node to use when several run on the same machine.

If no node is running, the command prints an error and exits with code 1.
