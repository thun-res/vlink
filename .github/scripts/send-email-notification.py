#!/usr/bin/env python3
import argparse
import os
import smtplib
import sys
from email.message import EmailMessage
from pathlib import Path


def env_value(*names: str) -> str:
    for name in names:
        value = os.environ.get(name, "").strip()
        if value:
            return value
    return ""


def bool_env(name: str, default: bool) -> bool:
    value = os.environ.get(name, "").strip().lower()
    if not value:
        return default
    return value in {"1", "true", "yes", "on"}


def main() -> int:
    parser = argparse.ArgumentParser(description="Send a short CI notification email.")
    parser.add_argument("--subject", required=True)
    parser.add_argument("--body", default="")
    parser.add_argument("--body-file", default="")
    args = parser.parse_args()

    host = env_value("VLINK_NOTIFY_SMTP_HOST", "SMTP_HOST")
    port = int(env_value("VLINK_NOTIFY_SMTP_PORT", "SMTP_PORT") or "587")
    username = env_value("VLINK_NOTIFY_SMTP_USERNAME", "SMTP_USERNAME", "SMTP_USER")
    password = env_value("VLINK_NOTIFY_SMTP_PASSWORD", "SMTP_PASSWORD", "SMTP_PASS")
    sender = env_value("VLINK_NOTIFY_EMAIL_FROM", "SMTP_FROM", "EMAIL_FROM")
    recipients = env_value("VLINK_NOTIFY_EMAIL_TO", "SMTP_TO", "EMAIL_TO")

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

    body = args.body
    if args.body_file:
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
