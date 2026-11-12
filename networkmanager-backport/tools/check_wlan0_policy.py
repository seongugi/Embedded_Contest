#!/usr/bin/env python3
"""Read-only check that wlan0 stays unmanaged when NetworkManager restarts."""
import configparser
import re
import subprocess
import sys


def selector(spec):
    """Evaluate interface-name selectors; return None for other selectors."""
    if spec == "*":
        return True
    if spec.startswith("interface-name:"):
        pattern = spec.partition(":")[2]
        if pattern.startswith("="):
            return pattern[1:] == "wlan0"
        pattern = re.escape(pattern.removeprefix("~"))
        return re.fullmatch(pattern.replace(r"\*", ".*").replace(r"\?", "."), "wlan0") is not None
    if ":" not in spec and not spec.startswith("except:"):
        return spec == "wlan0"
    return None


def matches(value):
    positive, negative = [], []
    for token in re.split(r"[;,]", value):
        token = token.strip()
        if not token:
            continue
        if token.startswith("except:"):
            negative.append(selector(token[7:]))
        else:
            positive.append(selector(token))
    if True in negative:
        return False
    if None in negative:
        return None
    if True in positive or (negative and not positive):
        return True
    return None if None in positive else False


def has_persistent_exclusion(text):
    cfg = configparser.ConfigParser(interpolation=None, strict=False)
    cfg.read_string(text)
    unmanaged = cfg.get("keyfile", "unmanaged-devices", fallback="")
    if matches(unmanaged) is True:
        return True

    # Accept unambiguous per-device rules; reject potentially competing rules.
    found = False
    for section in cfg.sections():
        if section != "device" and not section.startswith("device-"):
            continue
        applies = matches(cfg.get(section, "match-device", fallback="*"))
        if applies is False:
            continue
        if applies is None:
            return False
        managed = cfg.get(section, "managed", fallback="").lower()
        if managed in ("0", "no", "false"):
            found = True
        elif managed or cfg.getboolean(section, "stop-match", fallback=False):
            return False
    return found


def main():
    result = subprocess.run(
        ["NetworkManager", "--print-config"],
        check=True, text=True, stdout=subprocess.PIPE,
    )
    if not has_persistent_exclusion(result.stdout):
        sys.exit(
            "ERROR: An explicit persistent wlan0 exclusion was not found in "
            "NetworkManager configuration. Installation stopped without changes. "
            "Expected an existing keyfile.unmanaged-devices rule for wlan0 "
            "or a device section matching wlan0 with managed=0."
        )
    print("PASS: wlan0 has a persistent NetworkManager exclusion.")


if __name__ == "__main__":
    main()
