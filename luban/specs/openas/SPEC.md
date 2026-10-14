# openas Specification, version 0.1

## 1. Scope

openas describes actors -- people, agents, programs and services -- and what
they do with a system: who did it, for whom, what kind of effect it had, and
how a run is reported. A system that serves several kinds of actor describes
its verbs once and reports every run the same way; a person and an agent
differ in who answers a question, not in which verbs they have.

## 2. Actors

2.1 An actor has a kind -- `person`, `agent`, `program`, `service` -- and may
have a name.

2.2 An actor may act for others. `via` lists them, the outermost first: an
agent a person started is `agent:planner;via=person:alice`; a helper that
agent started is `agent:reader;via=person:alice,agent:planner`.

2.3 Delegation only adds to the chain. An actor acts for every actor in its
chain, and for no one else.

2.4 The text form is `<kind>[:<name>][;via=<actor>,<actor>...]`; a name is
letters, digits and `-_.@`.

2.5 An actor is inherited, not declared: what a process starts acts as that
process's actor, or as a delegate of it.

## 3. Effects

3.1 Every verb has one effect class:

| Effect | Meaning |
|---|---|
| `read` | changes nothing |
| `reversible` | changes what the system can take back |
| `irreversible` | removes or overwrites what cannot be taken back |
| `external` | reaches outside the system: the host, the network, a device |

3.2 A verb whose effect is `irreversible` or `external` asks before it runs,
unless the one running it has already answered.

3.3 A run that needs an answer no one present can give ends with exit code 2
and names the option that answers it. It never waits.

## 4. Verbs

A system describes each verb it offers: its name, a summary, its effect, its
arguments (by position), its options (by name; a flag takes no value) and the
options that answer its questions.

## 5. The envelope

5.1 A run is reported as lines, each one JSON object, with `kind`:

| kind | Fields |
|---|---|
| `phase` | `phase`, `state` (`begin`, `done`, `failed`), `text` |
| `progress` | `phase`, `percent` (0..1, or absent), `text` |
| `log` | `level`, `text` |
| `notice` | `text`, `hint` |
| `error` | `text`, `hint` (the step that helps), `code` |
| `data` | `data_kind`, `data` |
| `result` | `exitCode`; and `actor`, `trace` |

5.2 The last line is `result`. Exit code 0 is done, 1 failed, 2 needs an
answer (§3.3).

5.3 Fields are only ever added. A reader skips a kind or a field it does not
know.

5.4 A description of the verbs is one object: `openas` (this version),
`client`, `version`, `verbs`.

## 6. Conformance

`openas::conformance::check(verbs)` holds that:

6.1 a verb's name is lower case words;

6.2 names are unique;

6.3 every verb says what it does;

6.4 an argument or option is named once;

6.5 an answering option is one of the verb's options;

6.6 a verb that asks first names the option that answers.

## 7. Matters this version does not settle

Tokens and their scopes, budgets, stopping an actor, and the record of runs
beyond the `actor` and `trace` of a result.
