# TSDev Automation

Automates management of the TradeStation Development Environment (TSDev) Library by **driving TSDev.exe itself**: triggering the same code paths its UI controls trigger, without GUI automation and without requiring a visible window.

**This branch (`exp/remote-control`) is an overhaul.** The previous approach — interfacing with the TS library directly, without TSDev.exe — is discarded, and most of the existing codebase will be replaced.

- Requirements (SSOT): [AGENTS.md](AGENTS.md)
- TSDev.exe knowledge base (SSOT): [docs/KNOWLEDGE_BASE.md](docs/KNOWLEDGE_BASE.md)
- PM working memory: [docs/WORKING_MEMORY.md](docs/WORKING_MEMORY.md)

CLI and Python package documentation will be written here as the new implementation lands.
