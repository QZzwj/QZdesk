import argparse
import json
from pathlib import Path

from playwright.sync_api import sync_playwright


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--channel", default=None)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    output = Path(args.output)
    output.mkdir(parents=True, exist_ok=True)
    html = (Path(__file__).resolve().parents[1] / "xiaozhi_core/web/skills.html").read_text(encoding="utf-8")
    fixture = {
        "ok": True, "skills": [], "installed": [], "builtin": [],
        "summary": {"total": 0, "primary": 0, "secondary": 0},
        "messages": [], "devices": [], "connected": False,
        "status": "unavailable", "city": "模拟数据", "available": False,
        "error": "模拟器未连接真实服务",
    }
    with sync_playwright() as playwright:
        browser = playwright.chromium.launch(headless=True, channel=args.channel)
        try:
            for width, height in [(320, 240), (480, 320), (1440, 900)]:
                page = browser.new_page(viewport={"width": width, "height": height})
                errors = []
                page.on("pageerror", lambda error: errors.append(str(error)))
                state = {"reject": True}

                def route_request(route):
                    url = route.request.url
                    if url.endswith("/"):
                        route.fulfill(status=200, content_type="text/html", body=html)
                    elif "/api/chat/stream" in url:
                        route.fulfill(status=204, body="")
                    elif "/api/chat/send" in url:
                        route.fulfill(status=503 if state["reject"] else 202,
                                      content_type="application/json",
                                      body=json.dumps({"ok": not state["reject"], "error": "模拟发送失败", "queued": True}))
                    else:
                        route.fulfill(status=200, content_type="application/json", body=json.dumps(fixture))

                page.route("**/*", route_request)
                page.goto("http://qzdesk.test/", wait_until="networkidle")
                if width < 768:
                    page.select_option("#compactView", "chat")
                page.locator("#chatInput").fill("发送失败后应保留的草稿")
                page.locator("#chatSend").click()
                page.wait_for_function("!document.querySelector('#chatInput').disabled")
                assert page.locator("#chatInput").input_value() == "发送失败后应保留的草稿"
                page.screenshot(path=str(output / f"web-{width}x{height}-send-failed.png"))
                state["reject"] = False
                page.locator("#chatSend").click()
                page.wait_for_function("document.querySelector('#chatInput').value === ''")
                assert errors == [], errors
                print(f"WEB {width}x{height}: rejected send preserves draft; accepted send clears draft; no runtime errors")
                page.close()
        finally:
            browser.close()


if __name__ == "__main__":
    main()
