import argparse
import base64
import hashlib
import hmac
import time
import urllib.parse


def make_token(product_id: str, device_name: str, device_key: str, days: int) -> str:
    et = int(time.time()) + days * 24 * 3600
    version = "2018-10-31"
    method = "sha256"
    res = f"products/{product_id}/devices/{device_name}"
    sign_src = f"{et}\n{method}\n{res}\n{version}"
    key = base64.b64decode(device_key)
    digest = hmac.new(key, sign_src.encode(), hashlib.sha256).digest()
    sign = base64.b64encode(digest).decode()
    return (
        f"version={version}"
        f"&res={urllib.parse.quote(res, safe='')}"
        f"&et={et}"
        f"&method={method}"
        f"&sign={urllib.parse.quote(sign, safe='')}"
    )


if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("--product", required=True, help="OneNET Product ID")
    p.add_argument("--device", required=True, help="OneNET Device Name")
    p.add_argument("--key", required=True, help="OneNET Device Key")
    p.add_argument("--days", type=int, default=30)
    args = p.parse_args()
    print(make_token(args.product, args.device, args.key, args.days))
