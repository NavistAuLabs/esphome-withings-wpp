#!/usr/bin/env python3
"""Print the BLE association secret (`kl`) for the devices on a Withings account.

Read-only. Logs in to the legacy Health Mate account API with an email address
and password, lists that account's own device associations, and prints the `kl`
each one carries. Nothing is written to the account, and nothing is sent to any
device.

The account must have an email-and-password login. A Sign-in-with-Apple or
Google account cannot authenticate here until a password is set on it -- see
the repository README for the conversion steps.

    python3 scripts/get_kl.py --email you@example.com
    python3 scripts/get_kl.py --email you@example.com --mac AA:BB:CC:DD:EE:FF

The password is taken from --password, else $WITHINGS_PASSWORD, else an
interactive prompt. Only its MD5 digest is sent, because that is what the
legacy API expects.
"""

import argparse
import getpass
import hashlib
import json
import os
import sys
import urllib.parse
import urllib.request

AUTH_URL = "https://scalews.withings.net/cgi-bin/auth"
ACCOUNT_URL = "https://scalews.withings.com/cgi-bin/account"
ASSOCIATION_URL = "https://scalews.withings.com/cgi-bin/association"

# App identification the legacy endpoints expect alongside a session id. These
# are not credentials and are not account-specific.
APP_PARAMS = {"appname": "hmw", "appliver": "5010005", "apppfm": "web"}

# `kl` is exactly 32 ASCII characters (the component validates this at compile
# time). The legacy API has carried it under more than one field name -- the
# one named exactly "kl", or any name containing "secret" -- so match on that
# shape as well as on the name.
KL_LENGTH = 32
KL_NAME = "kl"


def post(url, data):
    body = urllib.parse.urlencode(data).encode()
    request = urllib.request.Request(url, data=body, headers={"User-Agent": "Mozilla/5.0"})
    with urllib.request.urlopen(request, timeout=25) as response:
        return json.loads(response.read().decode())


def walk(obj, path=""):
    """Yield (path, scalar) for every scalar anywhere in a nested structure."""
    if isinstance(obj, dict):
        for key, value in obj.items():
            if not isinstance(value, (dict, list)):
                yield f"{path}/{key}", value
            yield from walk(value, f"{path}/{key}")
    elif isinstance(obj, list):
        for index, value in enumerate(obj):
            yield from walk(value, f"{path}[{index}]")


def find_kl(association):
    fallback = None
    for path, value in walk(association):
        if not isinstance(value, str):
            continue
        key = path.rsplit("/", 1)[-1].lower()
        if key != KL_NAME and "secret" not in key:
            continue
        if len(value) == KL_LENGTH:
            return value
        fallback = fallback or value
    return fallback


def login(email, password):
    digest = hashlib.md5(password.encode()).hexdigest()
    response = post(
        AUTH_URL,
        {
            "action": "login",
            "email": email,
            "hash": digest,
            "duration": "604800",
            "os": "ios",
            "osversion": "15.4",
            "appname": "wiscaleNG",
            "apppfm": "ios",
            "appliver": "5010005",
        },
    )
    session_id = response.get("body", {}).get("sessionid")
    if response.get("status") != 0 or not session_id:
        sys.exit(
            "login failed. An OAuth/SSO-only account cannot use this endpoint -- "
            f"set a password on it first. API said: {json.dumps(response)[:300]}"
        )
    return session_id


def account_ids(session_id):
    response = post(ACCOUNT_URL, {"sessionid": session_id, "action": "get", "enrich": "t", **APP_PARAMS})
    body = response.get("body", {})
    accounts = body.get("account")
    if isinstance(accounts, dict):
        accounts = [accounts]
    ids = [a.get("id") for a in (accounts or []) if a.get("id") is not None]
    if not ids:
        ids = [v for v in (body.get("id"), body.get("accountid")) if v is not None]
    return ids


def associations(session_id, ids):
    found = []
    for account_id in ids:
        response = post(
            ASSOCIATION_URL,
            {
                "sessionid": session_id,
                "accountid": account_id,
                "type": "-1",
                "enrich": "t",
                "action": "getbyaccountid",
                **APP_PARAMS,
            },
        )
        body = response.get("body")
        if isinstance(body, dict):
            found += body.get("associations", [])
    return found


def device_mac(association):
    properties = association.get("deviceproperties") or {}
    return properties.get("macaddress") or properties.get("mac") or association.get("macaddress")


def normalise_mac(mac):
    return mac.lower().replace("-", ":") if mac else None


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--email", default=os.environ.get("WITHINGS_EMAIL"), help="account email address")
    parser.add_argument("--password", default=os.environ.get("WITHINGS_PASSWORD"), help="account password")
    parser.add_argument("--mac", help="only print the device with this BLE address")
    args = parser.parse_args()

    email = args.email or input("Withings account email: ")
    password = args.password or getpass.getpass("Withings account password: ")

    session_id = login(email, password)
    found = associations(session_id, account_ids(session_id))
    if not found:
        sys.exit("no device associations on this account")

    wanted = normalise_mac(args.mac)
    printed = 0
    for association in found:
        mac = device_mac(association)
        if wanted and normalise_mac(mac) != wanted:
            continue
        properties = association.get("deviceproperties") or {}
        model = properties.get("model") or properties.get("modelid") or properties.get("type")
        kl = find_kl(association)
        print(f"model={model} mac={mac}")
        print(f"  kl = {kl}" if kl else "  kl = (not present on this association)")
        printed += 1

    if not printed:
        sys.exit(f"no association matched {args.mac}")
    print("\nPut the value in your ESPHome secrets.yaml as withings_scale_kl.")
    print("It is a credential: do not paste it into an issue, a log or a pull request.")


if __name__ == "__main__":
    main()
