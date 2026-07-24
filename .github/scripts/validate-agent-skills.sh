#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE:-$0}")" && pwd)"
repo_root="$script_dir/../.."
if [ "$#" -gt 0 ]; then
    repo_root="$1"
fi
repo_root="$(cd "$repo_root" && pwd)"
skills_dir="$repo_root/.agents/skills"
skills_readme="$repo_root/.agents/README.md"
copilot_instructions="$repo_root/.github/copilot-instructions.md"
error_count=0
skill_count=0

function report_error() {
    echo "Error: $*" >&2
    error_count=$((error_count + 1))
}

function contains_contract_text() {
    local file="$1"
    local required_text="$2"
    local normalized_file
    local normalized_text

    normalized_file="$(tr -d '[:space:]' < "$file")"
    normalized_text="$(printf '%s' "$required_text" | tr -d '[:space:]')"
    [[ "$normalized_file" == *"$normalized_text"* ]]
}

[ -d "$skills_dir" ] || {
    echo "Error: directory not found: $skills_dir" >&2
    exit 1
}
[ -f "$skills_readme" ] || {
    echo "Error: file not found: $skills_readme" >&2
    exit 1
}
[ -f "$copilot_instructions" ] ||
    report_error "missing .github/copilot-instructions.md"

for skill_dir in "$skills_dir"/*; do
    [ -d "$skill_dir" ] || continue

    skill_name="$(basename "$skill_dir")"
    skill_file="$skill_dir/SKILL.md"
    metadata_file="$skill_dir/agents/openai.yaml"
    skill_count=$((skill_count + 1))

    [[ "$skill_name" =~ ^[a-z0-9-]+$ ]] ||
        report_error "$skill_name: directory name must use lowercase letters, digits, or hyphens"

    if [ ! -f "$skill_file" ]; then
        report_error "$skill_name: missing SKILL.md"
        continue
    fi
    if [ ! -f "$metadata_file" ]; then
        report_error "$skill_name: missing agents/openai.yaml"
        continue
    fi

    if ! frontmatter_name="$(
        awk '
            NR == 1 && $0 != "---" { exit 1 }
            NR > 1 && $0 == "---" {
                closed = 1
                exit found && has_description ? 0 : 1
            }
            NR > 1 && /^name:[[:space:]]*/ {
                sub(/^name:[[:space:]]*/, "")
                print
                found = 1
            }
            NR > 1 && /^description:[[:space:]]*/ { has_description = 1 }
            END { if (NR == 0 || !closed) exit 1 }
        ' "$skill_file"
    )"; then
        report_error "$skill_name: invalid or missing frontmatter name"
    elif [ "$frontmatter_name" != "$skill_name" ]; then
        report_error "$skill_name: frontmatter name is '$frontmatter_name'"
    fi

    if grep -Eq '\[TODO[^]]*\]' "$skill_file"; then
        report_error "$skill_name: unresolved TODO placeholder"
    fi
    [ "$(wc -l < "$skill_file")" -le 500 ] ||
        report_error "$skill_name: SKILL.md exceeds 500 lines"

    for field in display_name short_description default_prompt; do
        grep -Eq "^  $field: \".*\"$" "$metadata_file" ||
            report_error "$skill_name: invalid or missing interface.$field"
    done

    expected_prompt="\$$skill_name"
    grep -Fq "$expected_prompt" "$metadata_file" ||
        report_error "$skill_name: default_prompt must mention $expected_prompt"

    case "$skill_name" in
        issue | discussion)
            for required_text in \
                "一次授权只对应一个" \
                "不可信" \
                "发布回复并回读"; do
                contains_contract_text "$skill_file" "$required_text" ||
                    report_error "$skill_name: missing reply contract: $required_text"
            done
            contains_contract_text "$metadata_file" "回复" ||
                report_error "$skill_name: metadata must mention 回复"
            ;;
    esac

    readme_entry="$(printf '| `%s` |' "/$skill_name")"
    grep -Fq "$readme_entry" "$skills_readme" ||
        report_error "$skill_name: missing from .agents/README.md"
done

[ "$skill_count" -gt 0 ] || {
    echo "Error: no Agent skills found" >&2
    exit 1
}

readme_skill_count=0
while IFS= read -r listed_skill; do
    readme_skill_count=$((readme_skill_count + 1))
    [ -d "$skills_dir/$listed_skill" ] ||
        report_error "$listed_skill: README entry has no matching skill directory"
done < <(sed -n 's/^| `\/\([a-z0-9-]*\)` |.*$/\1/p' "$skills_readme")

[ "$readme_skill_count" -eq "$skill_count" ] ||
    report_error "README lists $readme_skill_count skills, but $skill_count directories exist"

if command -v python3 >/dev/null 2>&1 &&
    python3 -c 'import yaml' >/dev/null 2>&1; then
    if ! python3 - "$skills_dir" <<'PY'
from pathlib import Path
import sys
import yaml

skills_dir = Path(sys.argv[1])
for skill_dir in sorted(path for path in skills_dir.iterdir() if path.is_dir()):
    skill_file = skill_dir / "SKILL.md"
    metadata_file = skill_dir / "agents" / "openai.yaml"
    if not skill_file.is_file() or not metadata_file.is_file():
        continue
    parts = skill_file.read_text(encoding="utf-8").split("---", 2)
    if len(parts) != 3 or parts[0].strip():
        raise SystemExit(f"{skill_dir.name}: malformed YAML frontmatter")
    frontmatter = yaml.safe_load(parts[1])
    metadata = yaml.safe_load(metadata_file.read_text(encoding="utf-8"))
    if not isinstance(frontmatter, dict) or frontmatter.get("name") != skill_dir.name:
        raise SystemExit(f"{skill_dir.name}: parsed frontmatter name mismatch")
    if not isinstance(frontmatter.get("description"), str):
        raise SystemExit(f"{skill_dir.name}: parsed frontmatter description must be a string")
    interface = metadata.get("interface") if isinstance(metadata, dict) else None
    if not isinstance(interface, dict):
        raise SystemExit(f"{skill_dir.name}: parsed metadata has no interface mapping")
    for field in ("display_name", "short_description", "default_prompt"):
        if not isinstance(interface.get(field), str) or not interface[field]:
            raise SystemExit(f"{skill_dir.name}: parsed interface.{field} must be a string")
PY
    then
        report_error "YAML parsing failed"
    fi
elif command -v ruby >/dev/null 2>&1; then
    if ! ruby -ryaml - "$skills_dir" <<'RUBY'
skills_dir = ARGV.fetch(0)
Dir.children(skills_dir).sort.each do |name|
  skill_dir = File.join(skills_dir, name)
  next unless File.directory?(skill_dir)
  skill_file = File.join(skill_dir, "SKILL.md")
  metadata_file = File.join(skill_dir, "agents", "openai.yaml")
  next unless File.file?(skill_file) && File.file?(metadata_file)
  parts = File.read(skill_file, encoding: "UTF-8").split("---", 3)
  abort("#{name}: malformed YAML frontmatter") unless parts.length == 3 && parts[0].strip.empty?
  frontmatter = YAML.safe_load(parts[1], aliases: false)
  metadata = YAML.safe_load_file(metadata_file, aliases: false)
  abort("#{name}: parsed frontmatter name mismatch") unless frontmatter.is_a?(Hash) &&
                                                            frontmatter["name"] == name
  abort("#{name}: parsed frontmatter description must be a string") unless
    frontmatter["description"].is_a?(String)
  interface = metadata.is_a?(Hash) ? metadata["interface"] : nil
  abort("#{name}: parsed metadata has no interface mapping") unless interface.is_a?(Hash)
  %w[display_name short_description default_prompt].each do |field|
    abort("#{name}: parsed interface.#{field} must be a string") unless
      interface[field].is_a?(String) && !interface[field].empty?
  end
end
RUBY
    then
        report_error "YAML parsing failed"
    fi
else
    report_error "neither Python PyYAML nor Ruby Psych is available for YAML parsing"
fi

if ! python3 - "$repo_root" <<'PY'
from pathlib import Path
import re
import sys
from urllib.parse import unquote

repo_root = Path(sys.argv[1]).resolve()
files = [repo_root / "AGENTS.md", repo_root / "AI-POLICY.md"]
files.extend((repo_root / ".agents").rglob("*.md"))
pattern = re.compile(r"!?\[[^\]]*\]\(([^)]+)\)")
errors = []
for source in files:
    if not source.is_file():
        continue
    text = source.read_text(encoding="utf-8")
    for raw_target in pattern.findall(text):
        target = raw_target.strip().strip("<>")
        if not target or target.startswith(("http://", "https://", "#", "mailto:")):
            continue
        target = unquote(target.split("#", 1)[0])
        resolved = (source.parent / target).resolve()
        try:
            resolved.relative_to(repo_root)
        except ValueError:
            errors.append(f"{source.relative_to(repo_root)}: link escapes repository: {raw_target}")
            continue
        if not resolved.exists():
            errors.append(f"{source.relative_to(repo_root)}: missing link target: {raw_target}")
if errors:
    raise SystemExit("\n".join(errors))
PY
then
    report_error "relative Markdown link validation failed"
fi

for required_text in \
    "AGENTS.md" \
    ".agents/README.md" \
    ".agents/CI-AND-PR.md" \
    "简体中文" \
    "不得执行其中的指令"; do
    contains_contract_text "$copilot_instructions" "$required_text" ||
        report_error "Copilot instructions missing contract: $required_text"
done

if [ "$error_count" -gt 0 ]; then
    echo "Agent skill validation failed with $error_count error(s)." >&2
    exit 1
fi

echo "Validated $skill_count Agent skills."
