# Shyake PLAN

**Libraries**:

- libshyake (C)
- libshyake-go (Go)
- libshyake-rs (Rust)

**Clients**:

| Client | Language | UI | Platform |
|---|---|---|---|
| Shyake | C | CLI | POSIX |
| CocoaShyake | Obj-C | AppKit | macOS |
| NTShyake | C | Win32 | Windows |
| GTKShyake | C | GTK3 | Linux, BSD |
| HShyake | Go, TypeScript | HTML5, CSS3 | multi |
| GPShyake | Rust | GPUI | multi |

**Servers**:

| Server | Language | Notes |
|---|---|---|
| shyake-server-cf | TypeScript | Use Cloudflare Workers and D1. |
| shyake-server-go | Go | For self-hosting. Use SQLite |

**Codebase Structure**:

```
shyake/
├── docs/
│   └── i18n/
├── conformance/
├── lib/
│   ├── c           # libshyake
│   ├── go          # libshyake-go
│   └── rust        # libshyake-rs
├── client/
│   ├── posix       # Shyake
│   ├── osx         # CocoaShyake
│   ├── win32       # NTShyake
│   ├── gtk         # GimpShyake
│   ├── webui       # HShyake
│   └── gpui        # GPShyake
├── server/
│   ├── cf          # shyake-server-cf
│   └── go          # shyake-server-go
└── SPEC.md
```
