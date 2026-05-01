# Issue Tracker: GitHub

Issues and PRDs for this repo live in GitHub Issues for `sangimed/romm-vita-sync`.

Prefer the connected GitHub app tools when they are available in the current agent environment. If the GitHub app is unavailable and the `gh` CLI is installed, use `gh` from inside this repository clone so it can infer the repo from `git remote -v`.

## Conventions

- **Create an issue**: use the GitHub app issue creation tool, or `gh issue create --title "..." --body "..."`.
- **Read an issue**: use the GitHub app issue read/list tools, or `gh issue view <number> --comments`.
- **List issues**: use the GitHub app issue list/search tools, or `gh issue list --state open --json number,title,body,labels,comments`.
- **Comment on an issue**: use the GitHub app issue comment/update tools, or `gh issue comment <number> --body "..."`.
- **Apply or remove labels**: use the GitHub app label tools, or `gh issue edit <number> --add-label "..."` / `--remove-label "..."`.
- **Close an issue**: use the GitHub app issue update tool, or `gh issue close <number> --comment "..."`.

## When a skill says "publish to the issue tracker"

Create a GitHub issue in `sangimed/romm-vita-sync`.

## When a skill says "fetch the relevant ticket"

Fetch the GitHub issue by number and include comments and labels when they affect the task.
