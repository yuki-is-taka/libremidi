# libremidi4ue: integration branch for Libremidi4UE

This branch exists only in the fork `yuki-is-taka/libremidi`. It is what the
Libremidi4UE plugin pins as its `Source/ThirdParty/libremidi/libremidi`
submodule: upstream `master` plus the fix topics listed below. This file is
never part of a topic branch, so it never reaches an upstream submission.

## Branch model

- `master` mirrors upstream `celtera/libremidi` `master`. It is only ever
  fast-forwarded; nothing is committed to it.
- Each fix unit lives on its own topic branch, based on upstream `master` or
  on a declared prerequisite topic. Topics are the source of truth; each
  unit's tests sit in their own commit before the fix, so the tests survive
  when the fix is dropped in favour of an upstream equivalent.
- `libremidi4ue` is rebuilt from scratch on every upstream sync: upstream
  `master`, then a `--no-ff` merge of each topic in the order below, then
  this file.
- Every SHA that Libremidi4UE pins gets a tag `libremidi4ue-pin-YYYYMMDD[-x]`
  pushed to the fork, so `git submodule update` on an old Libremidi4UE
  commit keeps working after a rebuild. Tags are never moved or deleted.
- Upstream submissions are filed by the owner, from the topic branches.

## Topics, in merge order

| # | Topic | Based on | Content | Upstream |
|---|---|---|---|---|
| 1 | `fix/ump-reserved-sizes` | upstream `master` | Reserved UMP message type sizes, truncated tails, clamped copy into `ump::data[4]` (upstream pull request 264, carried unchanged with `cherry-pick -x`), preceded by extra bounds tests. | Follow upstream. When 264 merges, the rebase drops the carried commit as patch-equivalent; the tests commit stays. If upstream merges a variant, take upstream's and re-run the tests. |
| 2 | `fix/cmidi2-sysex7-bound` | upstream `master` | Bounded SysEx7 stack buffer in cmidi2's UMP -> MIDI 1 conversion; byte count bounded to 6. | Candidate for submission. |
| 3 | `fix/winmidi-batch-dispatch` | topic 1 | winmidi input: WinRT-free `dispatch_ump_batch` (`backends/winmidi/ump_batch.hpp`) walks each service batch per message; per-message group verdict; groupless and reserved types dropped on group-filtered ports (own commit); multi-group block range. Upstream issue 234. | Candidate for submission (the groupless-policy commit can be left out). If upstream fixes the batch handling first, drop the fix commits and keep the refactor and tests if they still apply. |
| 4 | `fix/midi1-sysex7-reassembly` | topic 3 | SysEx7 reassembly across packets for a MIDI 1 user on a UMP backend (`sysex7_to_midi1`). | Candidate for submission. |
| 5 | `fix/winmidi-output-group` | topic 3 | winmidi output: stamp the port's group (restamp outside the block's range, guarded by a host-to-device block covering the group); MIDI 1.0 converter group. Interim. | Fork-only until the port-identity redesign. |

## Rebuild on an upstream sync

Run in a clone with `origin` = the fork and `upstream` = `celtera/libremidi`
(fetch only). Resolve any conflict inside the topic it belongs to.

```sh
set -eu
git fetch upstream
git fetch origin

# 0. Mirror upstream master.
git push origin upstream/master:refs/heads/master

# 1. Rebase the topics, in dependency order.
old_dispatch=$(git rev-parse fix/winmidi-batch-dispatch)
git rebase --update-refs upstream/master fix/midi1-sysex7-reassembly   # topics 1, 3, 4
git rebase --onto fix/winmidi-batch-dispatch "$old_dispatch" fix/winmidi-output-group   # topic 5
git rebase upstream/master fix/cmidi2-sysex7-bound                     # topic 2

# 2. Drop-and-follow check: "-" marks a commit upstream already has.
for t in fix/ump-reserved-sizes fix/cmidi2-sysex7-bound fix/winmidi-batch-dispatch \
         fix/midi1-sysex7-reassembly fix/winmidi-output-group; do
  echo "== $t"; git cherry -v upstream/master "$t"
done

# 3. Recreate the integration branch.
notes=$(git rev-parse libremidi4ue)
git switch -C libremidi4ue upstream/master
for t in fix/ump-reserved-sizes fix/cmidi2-sysex7-bound fix/winmidi-batch-dispatch \
         fix/midi1-sysex7-reassembly fix/winmidi-output-group; do
  git merge --no-ff -m "libremidi4ue: merge $t" "$t"
done
git checkout "$notes" -- LIBREMIDI4UE.md
git commit -m "libremidi4ue: integration notes"
```

Then verify (below), tag and push:

```sh
tag=libremidi4ue-pin-$(date +%Y%m%d)        # add -b, -c ... for a second pin that day
git tag -a "$tag" -m "Libremidi4UE pin" libremidi4ue
git push --force-with-lease origin \
  fix/ump-reserved-sizes fix/cmidi2-sysex7-bound fix/winmidi-batch-dispatch \
  fix/midi1-sysex7-reassembly fix/winmidi-output-group libremidi4ue
git push origin "refs/tags/$tag"
```

To drop a unit, remove its topic from both loops (and from this table), and
rebase any topic based on it onto that topic's own base.

## Verification per rebuild

- Mac: CMake with `-DLIBREMIDI_TESTS=ON`, in three configurations: library
  mode, library mode with `-fsanitize=address,undefined`, and
  `-DLIBREMIDI_HEADER_ONLY=ON` (the mode Libremidi4UE uses) with the same
  sanitizers. Run `ctest`. Known environment failure: `midiout_test`
  expects the JACK API, which is not installed on the Mac.
- Windows: the winmidi backend headers (`midi_in.hpp`, `midi_out.hpp`)
  compile only there; build Libremidi4UE for Win64 and run the libremidi
  tests with MSVC, then the hardware probe.

## GitHub Actions

Actions are enabled on the fork. Upstream's workflows run on pushes to
`master` and `v*` tags; topic branches, `libremidi4ue` and
`libremidi4ue-pin-*` tags do not trigger them.
