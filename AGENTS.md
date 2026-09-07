# ae5-linux

(project notes go here)

# One brain file per repo — how to onboard

`AGENTS.md` = single source of truth. `CLAUDE.md` = one line: `@AGENTS.md`.
Move existing CLAUDE.md content into AGENTS.md, then paste the block below into AGENTS.md.

---

## Git Workflow (Aegiris standard — MANDATORY)

- NEVER commit to main/master (Claude surfaces hook-block edits there). First action when editing code: `git checkout -b <surface>/<MMDD>-<slug>` — surface: `cc`=Claude Code, `cd`=Claude Desktop, `cx`=Codex, `eve`=cloud. Use the Linear key in the slug when one exists (e.g. `cx/AEG-42-auth-fix`).
- Your session workspace is `G:\.workspace\project\<project>\<task-id>\` (Claude sessions get it auto-created and announced; otherwise create it). ALL plans, notes, reports, scratch go there — never in this repo.
- Isolated/parallel work: `git worktree add <workspace>\work -b <surface>/<slug>` and edit inside `work\`.
- Micro-commits (one logical step each). Push the branch after every work block. Never force-push. Never merge to main — Kyle merges.
- New project from chat: run `G:\scripts\project-new.ps1 -Name <name>`.
- Full standard: `G:\docs2\AGENT_GIT_WORKFLOW.md`.


