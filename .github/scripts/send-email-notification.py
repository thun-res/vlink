#!/usr/bin/env python3
import argparse
import os
import smtplib
import sys
from email.message import EmailMessage
from pathlib import Path


def bool_env(name: str, default: bool) -> bool:
    value = os.environ.get(name, "").strip().lower()
    if not value:
        return default
    return value in {"1", "true", "yes", "on"}


def main() -> int:
    parser = argparse.ArgumentParser(description="Send a short CI notification email.")
    parser.add_argument("--subject", required=True)
    parser.add_argument("--body-file", required=True)
    args = parser.parse_args()

    host = os.environ.get("VLINK_NOTIFY_SMTP_HOST", "").strip()
    port = int(os.environ.get("VLINK_NOTIFY_SMTP_PORT", "").strip() or "587")
    username = os.environ.get("VLINK_NOTIFY_SMTP_USERNAME", "").strip()
    password = os.environ.get("VLINK_NOTIFY_SMTP_PASSWORD", "").strip()
    sender = os.environ.get("VLINK_NOTIFY_EMAIL_FROM", "").strip()
    recipients = os.environ.get("VLINK_NOTIFY_EMAIL_TO", "").strip()

    missing = [
        name
        for name, value in (
            ("VLINK_NOTIFY_SMTP_HOST", host),
            ("VLINK_NOTIFY_EMAIL_FROM", sender),
            ("VLINK_NOTIFY_EMAIL_TO", recipients),
        )
        if not value
    ]
    if missing:
        print(f"Email notification skipped: {', '.join(missing)} is not configured.")
        return 0
    if bool(username) != bool(password):
        print("Email notification skipped: SMTP username and password must be configured together.")
        return 0

    body = Path(args.body_file).read_text(encoding="utf-8")

    message = EmailMessage()
    message["Subject"] = args.subject
    message["From"] = sender
    message["To"] = recipients
    message.set_content(body)

    use_ssl = bool_env("VLINK_NOTIFY_SMTP_SSL", False)
    use_starttls = bool_env("VLINK_NOTIFY_SMTP_STARTTLS", not use_ssl)
    smtp_cls = smtplib.SMTP_SSL if use_ssl else smtplib.SMTP

    with smtp_cls(host, port, timeout=30) as smtp:
        if use_starttls and not use_ssl:
            smtp.starttls()
        if username:
            smtp.login(username, password)
        smtp.send_message(message)

    print("Email notification sent.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
