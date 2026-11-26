# dsm_cli

Queries the cluster through the DSM node running on this machine.

```sh
./build/dsm_cli [--node <id>] <command>
```

| Command | Shows |
| --- | --- |
| `status` | Every node of the cluster, online or offline |
| `stats` | Statistics of the local node |
| `stats <id>` | Statistics of node `<id>` |
| `stats all` | Statistics of every active node and the totals of the system |
| `pages <id>` | Page table of node `<id>` |
| `pages all` | Page tables of every active node |
| `read <address>` | The 8-byte value stored at a shared address, in decimal and hexadecimal |

In a page table, `PROCESS`, `ORDER` and `COPIES` are known only by the home node of the page. Rows of
pages whose home is another node show `-` in those columns.

`read` first checks in the page table of the home node that the address is allocated, then reads it like
any application, so the node of the CLI keeps a shared copy of that page. It refuses addresses outside the
shared window, addresses that are not allocated, and 8 bytes that cross the end of a page.

`--node <id>` picks the local node to use when several run on the same machine.

If no node is running, the command prints an error and exits with code 1.
