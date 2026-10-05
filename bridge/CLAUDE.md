# The Sims 3 under RTX Remix

## What this is
A compatibility hook that lets NVIDIA RTX Remix ray-trace The Sims 3. The game's
renderer hands Remix nothing it can use, so a hook inside this fork of the Remix bridge
client recovers the camera, decides per draw what Remix captures, remaps textures,
feeds the terrain to Remix's baker and forwards the game's lights. It runs on this
Windows PC against a live game install with saves that matter and a live Remix runtime.
Every build is proven the same way: the user launches the game, plays, reads the logs
and reports what they see. A run costs real time, so a build that cannot show in the log
that its change engaged should not ask for one. The whole thing must stay replicable:
anything another person needs to install or build it lives in this repository, under
`sims3/`, not in notes that only exist on this machine.

## Hard rules
- Small, reversible steps. One change per build, one commit per build on the hook
  branch, the previous build and its configuration kept next to the new one so any step
  can be undone. Never push without asking.
- Nothing outside this repository changes without an exact description of what and a
  yes: the game install and its configuration, the Remix runtime and bridge server,
  saves, mods, drivers, the registry. The one routine exception is deploying a freshly
  built client DLL and its option file into the game folder, once the game is closed.
- Understand before changing. Find the code path, the log line or the bytecode that
  shows the cause; do not change things to see what happens. Before asking for a run,
  prove from the existing logs or the code that the change will engage, and ship it with
  the log line or test that will show it did. When a diagnosis turns out wrong, revert
  its code in the next build rather than leaving it in.
- Back up a configuration file before editing it, next to the file, named with the date
  and the reason. Re-read a file before editing it: the runtime rewrites its own.
- Never launch, kill or restart the game. Never download anything without asking.
- Every run's logs and screenshots are archived before the next build.

## Design rules for the hook
Since the simplification study (milestones 144-164) every build works toward these. A
build that would break one says so, and why, before it is written.
- Facts from the game. What the hook needs to know about a draw -- which texture is the
  colour, which coordinate feeds it, what a shader cuts, where a tree's fade arrives -- is
  read from the shader bytecode or the game's own records. A hand table holds only what
  analysis cannot know (which shaders are the game's own fakes, which material a glass
  gets), and each entry says why.
- One mechanism per job. Extend the existing one instead of adding a second: the hook's
  own calls and their undo (HookCalls), sending a draw (sims3SendDraw), caches
  (Cache<E,N>), geometry (sims3GeometryOf), textures (sims3MakeTexture, decodeColour),
  options (kHookOptions), dropping draws (kDropPs), alpha tests (alphaTestFor,
  sims3SetAlphaTest), the colour (chooseAutoAlbedo). Renaming one updates this list in
  the same commit.
- No fallbacks. One path per job. A failed precondition is logged and counted, not
  served by a second method. A replaced method goes with everything only it needed: its
  state, logs, checks and statistics.
- Logs report state: the periodic statistics, and one line per shader the first time
  it matters. No per-event logging. Diagnostics go once answered.
- A rule is scoped to where it is exact. Before building one, run it over every dumped
  shader and count what it changes; keep it to the shaders where the bytecode proves it.
  Each rule gets a unit test on the real dumps.
- A table goes in two builds: first the rule beside the table, with a check that logs
  where they disagree; after a run shows they agree, the rule takes over and the table and
  check go.

## How to work with me
- Be honest. If there is a better way, or I am overcomplicating something, say so
  before doing it my way.
- Report faithfully: failures with their output, mistakes named together with what you
  did about them, never a check you did not run. When tests pass, say so plainly.
- Findings are written simple and certain: what we know, in a form that pastes into an
  issue. Speculation gets one hedged sentence or none.
- When I ask a question or describe a problem, answer or assess first; act when I say so.
- Highlight what you notice along the way, once and in proportion, and record it in
  memory rather than pitching it again. If something we find could fix or improve
  another part of the hook or the setup, suggest it.
- Prefer the simplest thing that works. No wrappers, scripts or automation I did not
  ask for. Diagnostic code is removed once it has answered its question.
- Keep memory current: when something changes, update the relevant note in the same
  session.

## Memory
Read MEMORY.md first, then the project inventory note.
