# XLINGS_RES Configuration

`XLINGS_RES` is the config field used to rewrite package resources whose package metadata declares `url = "XLINGS_RES"`.

## Supported Shapes

1. Single default server

```json
{
  "XLINGS_RES": "https://gitcode.com/xlings-res"
}
```

2. Default server list

```json
{
  "XLINGS_RES": [
    "https://gitcode.com/xlings-res",
    "https://github.com/xlings-res"
  ]
}
```

3. Region-aware object

```json
{
  "mirror": "CN",
  "XLINGS_RES": {
    "default": "https://github.com/xlings-res",
    "CN": [
      "https://gitcode.com/xlings-res",
      "https://mirror.example.com/xlings-res"
    ]
  }
}
```

Each region value may be either a string or a list.

## Match Priority

When resolving the actual resource server for `XLINGS_RES`, the selection order is:

1. Project `.xlings.json`
2. Global `XLINGS_HOME/.xlings.json`
3. Built-in defaults in code

Within a single config file, the lookup order is:

1. Region-specific entry matching `mirror`
2. Default entry (`default` or `DEFAULT`)

This means project config is treated as a complete higher-priority override source:

- if project config has `XLINGS_RES.CN`, use it
- otherwise if project config has default `XLINGS_RES`, use it
- otherwise fall back to global config

Global config follows the same rule before falling back to code defaults.

## Multi-Server Selection

If the selected entry is a list, `xlings` probes the candidates and chooses a reachable server quickly:

- probe timeout is short to avoid blocking on bad endpoints
- if a candidate responds with connect latency under about `100ms`, it is accepted immediately
- otherwise the best reachable latency among the candidates is used

This is intended to prefer a fast server without turning install-time selection into a long blocking step.

## Current Built-in Defaults

- `GLOBAL` -> `https://github.com/xlings-res`
- `CN` -> `https://gitcode.com/xlings-res`
