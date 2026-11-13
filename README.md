# DSM - Distributed Shared Memory

User-space distributed shared memory for Linux with on-demand paging and MSI coherence.
Course project for IC-6600 Principios de Sistemas Operativos, TEC.

The architecture, API, message protocol and configuration are described in the
[design document](https://docs.google.com/document/d/1PIC0DqcMj6tocuHQD2VkpQ3Mr1XlRh2rHgJ85d4Ntgw).

## Run

Requires Linux x86_64, `g++` (C++17), `make` and `libssl-dev`.

```sh
make
make test
```

Terminal 1, node 0 writes a message in its shared memory:

```sh
./build/demo 0 write "hello from node 0"
```

Terminal 2, node 1 reads it through the DSM, using the address printed by node 0:

```sh
./build/demo 1 read 0x100000000000
```

Query a node:

```sh
./build/dsm_cli --config config/cluster.json status
```

All CLI commands are described in [docs/cli.md](docs/cli.md).

Commit dates were distributed between 2026-11-09 and 2026-11-13 with the professor's authorization.
