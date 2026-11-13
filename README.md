# DSM - Distributed Shared Memory

User-space distributed shared memory for Linux with on-demand paging and MSI coherence.
Course project for IC-6600 Principios de Sistemas Operativos, TEC.

The architecture, API, message protocol and configuration are described in the
[design document](https://docs.google.com/document/d/1PIC0DqcMj6tocuHQD2VkpQ3Mr1XlRh2rHgJ85d4Ntgw).

## Run

Requires Linux x86_64 (kernel 4.17 or newer), `g++` 8 or newer, `make` and `libssl-dev`.

```sh
make
make test
```

Start one node per terminal. Each node takes the first entry of `config/cluster.json` whose IP
belongs to the machine and whose ports are free:

```sh
./build/dsm_node
./build/dsm_node
```

Write a message through node 0 and read it through node 1:

```sh
./build/demo --node 0 write "hello from node 0"
./build/demo --node 1 read 0x100000000000
```

Query the cluster:

```sh
./build/dsm_cli status
```

All CLI commands are described in [docs/cli.md](docs/cli.md).

Third party: [nlohmann/json](https://github.com/nlohmann/json) 3.11.3 (MIT), vendored in `core/third_party`.
