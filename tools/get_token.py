#!/usr/bin/env python3
"""
Antigravity / Google AI Quota OAuth Token Helper
Retrieves, verifies, and optionally uploads the OAuth Refresh Token to the
ES3C28P Internet Radio (default mDNS host: es3c28p-radio.local).
"""

import argparse
import http.server
import json
import os
import socketserver
import sys
import threading
import urllib.error
import urllib.parse
import urllib.request
import webbrowser

DEFAULT_HOST = "es3c28p-radio.local"
USER_AGENT = "antigravity/cli/1.4.2 (aidev_client; os_type=linux; arch=amd64; cl=1001591392; auth_method=consumer)"
QUOTA_ENDPOINT = "https://daily-cloudcode-pa.googleapis.com/v1internal:retrieveUserQuotaSummary"
CLIENT_SECRET_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "client_secret.json")


def load_client_credentials():
    """Load OAuth client_id and client_secret from client_secret.json in the same directory."""
    if not os.path.exists(CLIENT_SECRET_FILE):
        print(f"[!] Error: '{CLIENT_SECRET_FILE}' not found.")
        print("    Please place 'client_secret.json' in the tools/ directory.")
        sys.exit(1)
    try:
        with open(CLIENT_SECRET_FILE, "r", encoding="utf-8") as f:
            data = json.load(f)
        # Support both Google Cloud standard format ("installed" / "web") and flat JSON format
        section = data.get("installed") or data.get("web") or data
        client_id = section.get("client_id")
        client_secret = section.get("client_secret")
        if not client_id or not client_secret:
            raise ValueError("Missing 'client_id' or 'client_secret' in JSON")
        return client_id, client_secret
    except Exception as e:
        print(f"[!] Failed to load credentials from '{CLIENT_SECRET_FILE}': {e}")
        sys.exit(1)


def try_read_keyring_token():
    """Attempt to retrieve existing Antigravity CLI token from OS secret service."""
    try:
        import dbus
        bus = dbus.SessionBus()
        service = bus.get_object("org.freedesktop.secrets", "/org/freedesktop/secrets")
        s_iface = dbus.Interface(service, "org.freedesktop.Secret.Service")
        _, session = s_iface.OpenSession("plain", dbus.String("", variant_level=1))
        results = s_iface.SearchItems({"service": "gemini", "username": "antigravity"})
        if results and len(results[0]) > 0:
            item = bus.get_object("org.freedesktop.secrets", results[0][0])
            secret = dbus.Interface(item, "org.freedesktop.Secret.Item").GetSecret(session)
            data = json.loads(bytes(secret[2]).decode())
            tok = data.get("token", {})
            return tok.get("refresh_token")
    except Exception:
        pass
    return None


def verify_and_print_quota(client_id, client_secret, refresh_token):
    """Verify refresh token and display live quota buckets."""
    print("\n[+] 1. Fetching Google OAuth Access Token...")
    body = urllib.parse.urlencode({
        "client_id": client_id,
        "client_secret": client_secret,
        "refresh_token": refresh_token,
        "grant_type": "refresh_token",
    }).encode()
    req = urllib.request.Request("https://oauth2.googleapis.com/token", data=body, method="POST")
    req.add_header("Content-Type", "application/x-www-form-urlencoded")
    try:
        with urllib.request.urlopen(req) as resp:
            token_res = json.loads(resp.read().decode())
            access_token = token_res["access_token"]
            print("    [OK] Access Token obtained successfully.")
    except Exception as e:
        print(f"    [!] Failed to refresh token: {e}")
        return False

    print("\n[+] 2. Querying Antigravity Quota API...")
    quota_req = urllib.request.Request(
        QUOTA_ENDPOINT,
        data=json.dumps({"project": "aicode-consumers"}).encode(),
        method="POST",
    )
    quota_req.add_header("Authorization", f"Bearer {access_token}")
    quota_req.add_header("Content-Type", "application/json")
    quota_req.add_header("User-Agent", USER_AGENT)

    try:
        with urllib.request.urlopen(quota_req) as resp:
            data = json.loads(resp.read().decode())
            print("    [OK] Quota data received successfully!\n")
            print("=" * 64)
            print("         Antigravity Live Quota Status")
            print("=" * 64)
            for grp in data.get("groups", []):
                grp_name = grp.get("displayName", "")
                for b in grp.get("buckets", []):
                    b_name = b.get("displayName", "")
                    frac = b.get("remainingFraction", 0.0)
                    pct = int(frac * 100)
                    bar = "#" * (pct // 5) + "-" * (20 - (pct // 5))
                    reset = b.get("resetTime", "")
                    print(f"[{grp_name}] {b_name}")
                    print(f"  [{bar}] {pct:>3}% (Reset: {reset})")
            print("=" * 64)
            return True
    except Exception as e:
        print(f"    [!] Failed to retrieve quota: {e}")
        return False


class _ReusableTCPServer(socketserver.TCPServer):
    allow_reuse_address = True


def interactive_browser_auth(client_id, client_secret):
    """Launch local webserver and initiate browser OAuth authorization flow."""
    print("\nOpening browser for Google OAuth sign-in...")
    auth_code = None
    server_done = threading.Event()

    class OAuthHandler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            nonlocal auth_code
            parsed = urllib.parse.urlparse(self.path)
            qs = urllib.parse.parse_qs(parsed.query)
            if "code" in qs:
                auth_code = qs["code"][0]
                self.send_response(200)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.end_headers()
                self.wfile.write(
                    b"<h1>Authentication successful!</h1>"
                    b"<p>You can close this tab now and return to your terminal.</p>"
                )
            else:
                self.send_response(400)
                self.end_headers()
            server_done.set()

        def log_message(self, format, *args):
            pass

    server = _ReusableTCPServer(("127.0.0.1", 8085), OAuthHandler)
    t = threading.Thread(target=server.serve_forever, daemon=True)
    t.start()

    params = {
        "client_id": client_id,
        "redirect_uri": "http://localhost:8085/",
        "response_type": "code",
        "scope": "https://www.googleapis.com/auth/userinfo.email https://www.googleapis.com/auth/cloud-platform https://www.googleapis.com/auth/cclog",
        "access_type": "offline",
        "prompt": "consent",
    }
    auth_url = "https://accounts.google.com/o/oauth2/v2/auth?" + urllib.parse.urlencode(params)
    print(f"\nIf browser does not open automatically, visit this URL:\n{auth_url}\n")
    webbrowser.open(auth_url)

    server_done.wait(timeout=120)
    server.shutdown()
    server.server_close()

    if not auth_code:
        print("[!] Authorization code was not received.")
        return None

    # Exchange code for refresh token
    body = urllib.parse.urlencode({
        "client_id": client_id,
        "client_secret": client_secret,
        "code": auth_code,
        "grant_type": "authorization_code",
        "redirect_uri": "http://localhost:8085/",
    }).encode()
    req = urllib.request.Request("https://oauth2.googleapis.com/token", data=body, method="POST")
    req.add_header("Content-Type", "application/x-www-form-urlencoded")
    with urllib.request.urlopen(req) as resp:
        res = json.loads(resp.read().decode())
        return res.get("refresh_token")


def _normalize_host(esp_host):
    host = esp_host.strip()
    if host.startswith("http://"):
        host = host[len("http://"):]
    elif host.startswith("https://"):
        host = host[len("https://"):]
    return host.rstrip("/")


def send_token_to_esp(client_id, client_secret, token, esp_host):
    """Send refresh token and OAuth credentials to ESP32-S3 via POST /api/agy (fallback POST /save)."""
    host = _normalize_host(esp_host)
    if not host:
        return False

    agy_url = f"http://{host}/api/agy"
    json_payload = json.dumps({
        "oauth_client_id": client_id,
        "oauth_client_secret": client_secret,
        "oauth_refresh_token": token,
    }).encode("utf-8")

    print(f"\n[+] Sending OAuth credentials (Token, Client ID, Secret) to {agy_url}...")
    try:
        req = urllib.request.Request(
            agy_url,
            data=json_payload,
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        with urllib.request.urlopen(req, timeout=8) as resp:
            if resp.status == 200:
                print("[SUCCESS] OAuth credentials successfully saved via /api/agy!")
                print("The Quota view will update automatically.")
                return True
            print(f"[!] /api/agy returned status {resp.status}")
    except urllib.error.HTTPError as e:
        if e.code not in (404, 405):
            print(f"[!] Server error from {agy_url}: HTTP {e.code} {e.reason}")
            return False
        print(f"[*] /api/agy not available (HTTP {e.code}), trying legacy /save...")
    except Exception as e:
        print(f"[!] Could not reach device at {host}: {e}")
        return False

    save_url = f"http://{host}/save"
    post_data = urllib.parse.urlencode({
        "refr": token,
        "cid": client_id,
        "sec": client_secret,
    }).encode("utf-8")
    try:
        req = urllib.request.Request(
            save_url,
            data=post_data,
            headers={"Content-Type": "application/x-www-form-urlencoded"},
            method="POST",
        )
        with urllib.request.urlopen(req, timeout=8) as resp:
            if resp.status == 200:
                print("[SUCCESS] OAuth credentials successfully saved via /save!")
                print("The Quota view will update automatically.")
                return True
            print(f"[!] /save returned status {resp.status}")
    except Exception as e:
        print(f"[!] Could not reach device at {save_url}: {e}")
    return False


def main():
    parser = argparse.ArgumentParser(
        description="Retrieve and upload Google AI Quota OAuth credentials to ES3C28P Internet Radio."
    )
    parser.add_argument(
        "--host",
        default=None,
        help=f"Target ESP32-S3 hostname or IP (e.g. {DEFAULT_HOST} or 192.168.4.1)",
    )
    parser.add_argument(
        "--no-upload",
        action="store_true",
        help="Skip uploading credentials to the device",
    )
    args = parser.parse_args()

    client_id, client_secret = load_client_credentials()

    print("=" * 64)
    print("   Antigravity / Google AI Pro OAuth Token Helper")
    print("=" * 64)

    token = try_read_keyring_token()
    if token:
        print("[+] Existing Antigravity CLI token detected automatically!")
    else:
        print("[*] No saved token found. Starting browser OAuth sign-in flow...")
        token = interactive_browser_auth(client_id, client_secret)

    if not token:
        print("[!] Failed to obtain token.")
        sys.exit(1)

    print("\n" + "#" * 64)
    print("  Your OAuth Refresh Token:")
    print(f"  {token}")
    print("#" * 64 + "\n")

    verify_and_print_quota(client_id, client_secret, token)

    print("\n[+] Next Steps:")
    print(" Option A (Browser Web UI):")
    print(f"   1. Open http://{DEFAULT_HOST}/ (or http://192.168.4.1/ in AP mode 'ES3C28P-Radio').")
    print("   2. Open the Clock / Quota settings section.")
    print("   3. Paste the Client ID, Client Secret, and Refresh Token above and save.")

    if args.no_upload:
        return

    print("\n Option B (Direct Send):")
    target_host = args.host
    if target_host is None:
        raw = input(
            f"   Enter device host/IP (e.g. {DEFAULT_HOST} or 192.168.4.1, press Enter for {DEFAULT_HOST}, or 'skip'): "
        ).strip()
        if raw.lower() in ("skip", "s", "n", "no"):
            return
        target_host = raw or DEFAULT_HOST

    send_token_to_esp(client_id, client_secret, token, target_host)


if __name__ == "__main__":
    main()
