# DSM - Distributed Shared Memory

User-space distributed shared memory for Linux with on-demand paging and MSI coherence.
Course project for IC-6600 Principios de Sistemas Operativos, TEC.

The architecture, API, message protocol and configuration are described in the
[design document](https://docs.google.com/document/d/1PIC0DqcMj6tocuHQD2VkpQ3Mr1XlRh2rHgJ85d4Ntgw).

## Run

Requires Linux x86_64 (kernel 4.17 or newer), `g++` 8 or newer and `make`.

```sh
make
```

Start one node per terminal. Each node takes the first entry of `config/cluster.json` whose IP
belongs to the machine and whose ports are free:

```sh
./build/dsm_node
./build/dsm_node
```

Each node offers the `memory_size` of its entry, and the memory of the cluster is the sum of all
of them. The node an application attaches to is the home of everything the application allocates.
When an allocation does not fit in the free memory of that node, the node stores the remaining pages
on other nodes, the one with the most free memory first, and the application still receives one
contiguous range of addresses. MSI then moves each page to the node that uses it. When an
application exits without freeing its memory, its node releases it.

Write a message through node 0 and, from another terminal, read it through node 1 with the address
printed by the writer. Keep the writer running while reading: pressing Enter frees the message.

```sh
./build/demo_v1 --node 0 write "hello from node 0"
./build/demo_v1 --node 1 read 0x100000000000
```

Distributed mutex and barrier. `counter` runs several threads that increment a shared counter
under one mutex. With a node count it prints the `join` command that every other node runs, so
threads of all nodes write the same page. `host` creates a mutex and a barrier and prints the
guest command, which another node runs to wait for the mutex while the host holds it.

```sh
./build/sync_demo --node 0 counter 4 2000
./build/sync_demo --node 0 counter 4 500 2
./build/sync_demo --node 1 join 0x100000000000 0x0 0x0 0x1 4 500
./build/sync_demo --node 0 host
./build/sync_demo --node 1 guest 0x0 0x0
```

Game of Life splits the grid into row slabs, one per node, and exchanges only the two halo rows
after each generation. Node 0 prints the exact join command for the other nodes:

```sh
./build/life --node 0 --nodes 3 --width 32768 --height 32768 --generations 100 --snapshot-every 25 --ppm /tmp/life
./build/life --node 1 --join 0x100000000000 --threads 4
./build/life --node 2 --join 0x100000000000 --threads 4
```

Use `--ascii` to render frames in the terminal instead of writing PPM files, and `--wrap` for a
toroidal grid. Each node stores two grid buffers plus a display buffer; a 32768 x 32768 grid with
three nodes needs roughly 720 MB of DSM memory per node. Raise the node memory in
`config/cluster.json` accordingly before running the full-size demo.

Query the cluster:

```sh
./build/dsm_cli status
```

All CLI commands are described in [docs/cli.md](docs/cli.md).

Third party, vendored in `core/third_party`: [nlohmann/json](https://github.com/nlohmann/json) 3.11.3 (MIT) and
[PicoSHA2](https://github.com/okdshin/PicoSHA2) (MIT) for the SHA-256 configuration hash.
