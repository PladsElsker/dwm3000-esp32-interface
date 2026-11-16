"""Durable SQLite storage for application messages."""
from __future__ import annotations

import sqlite3
import threading
import time


class MessageStore:
    def __init__(self, path: str | None):
        self.path = path
        self._lock = threading.RLock()
        if path is None:
            target = ":memory:"
        else:
            import os
            os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
            target = path
        self._db = sqlite3.connect(target, check_same_thread=False, isolation_level=None)
        with self._lock:
            self._db.execute("PRAGMA journal_mode=WAL")
            self._db.execute("PRAGMA synchronous=FULL")
            self._db.execute("""
                CREATE TABLE IF NOT EXISTS outbox (
                    message_id TEXT PRIMARY KEY, peer_id INTEGER NOT NULL,
                    kind TEXT NOT NULL, envelope BLOB NOT NULL,
                    attempts INTEGER NOT NULL DEFAULT 0, next_attempt REAL NOT NULL,
                    status TEXT NOT NULL DEFAULT 'pending', last_error TEXT
                )
            """)
            self._db.execute("""
                CREATE TABLE IF NOT EXISTS inbox (
                    peer_id INTEGER NOT NULL, message_id TEXT NOT NULL,
                    kind TEXT NOT NULL, envelope TEXT NOT NULL, received_at REAL NOT NULL,
                    processed INTEGER NOT NULL DEFAULT 0, dispatch_attempts INTEGER NOT NULL DEFAULT 0,
                    next_dispatch REAL NOT NULL DEFAULT 0, PRIMARY KEY(peer_id,message_id)
                )
            """)
            self._db.execute("""
                CREATE TABLE IF NOT EXISTS requests (
                    message_id TEXT PRIMARY KEY, peer_id INTEGER NOT NULL,
                    created_at REAL NOT NULL, accepted INTEGER NOT NULL DEFAULT 0,
                    response_json TEXT
                )
            """)
            self._db.execute("""
                CREATE TABLE IF NOT EXISTS responses (
                    peer_id INTEGER NOT NULL, request_id TEXT NOT NULL,
                    response_id TEXT NOT NULL, PRIMARY KEY(peer_id,request_id)
                )
            """)

    def enqueue(self, message_id: str, peer_id: int, kind: str, envelope: bytes):
        now = time.time()
        with self._lock:
            self._db.execute("BEGIN IMMEDIATE")
            try:
                self._db.execute(
                    "INSERT INTO outbox(message_id,peer_id,kind,envelope,next_attempt) VALUES(?,?,?,?,?)",
                    (message_id, peer_id, kind, envelope, now),
                )
                if kind == "request":
                    self._db.execute(
                        "INSERT OR REPLACE INTO requests(message_id,peer_id,created_at) VALUES(?,?,?)",
                        (message_id, peer_id, now),
                    )
                self._db.execute("COMMIT")
            except Exception:
                self._db.execute("ROLLBACK")
                raise

    def enqueue_response_once(self, peer_id: int, request_id: str,
                              response_id: str, envelope: bytes) -> bool:
        now = time.time()
        with self._lock:
            self._db.execute("BEGIN IMMEDIATE")
            try:
                cursor = self._db.execute(
                    "INSERT OR IGNORE INTO responses(peer_id,request_id,response_id) VALUES(?,?,?)",
                    (peer_id, request_id, response_id),
                )
                if cursor.rowcount != 1:
                    self._db.execute("ROLLBACK")
                    return False
                self._db.execute(
                    "INSERT INTO outbox(message_id,peer_id,kind,envelope,next_attempt) VALUES(?,?,?,?,?)",
                    (response_id, peer_id, "response", envelope, now),
                )
                self._db.execute("COMMIT")
                return True
            except Exception:
                self._db.execute("ROLLBACK")
                raise

    def due_outbox(self, now: float, limit: int = 64):
        with self._lock:
            return self._db.execute(
                """SELECT message_id,peer_id,kind,envelope,attempts FROM outbox
                   WHERE status='pending' AND next_attempt<=? ORDER BY next_attempt LIMIT ?""",
                (now, limit),
            ).fetchall()

    def mark_transport_attempt(self, message_id: str, attempts: int,
                               next_attempt: float, error: str | None = None):
        with self._lock:
            self._db.execute(
                "UPDATE outbox SET attempts=?,next_attempt=?,last_error=? WHERE message_id=?",
                (attempts, next_attempt, error, message_id),
            )

    def mark_failed(self, message_id: str, attempts: int, error: str):
        with self._lock:
            self._db.execute(
                "UPDATE outbox SET status='failed',attempts=?,last_error=? WHERE message_id=?",
                (attempts, error, message_id),
            )

    def acknowledge(self, peer_id: int, message_id: str):
        with self._lock:
            self._db.execute("BEGIN IMMEDIATE")
            try:
                row = self._db.execute(
                    "SELECT kind FROM outbox WHERE peer_id=? AND message_id=?",
                    (peer_id, message_id),
                ).fetchone()
                if not row:
                    self._db.execute("ROLLBACK")
                    return None
                kind = row[0]
                if kind == "request":
                    self._db.execute("UPDATE requests SET accepted=1 WHERE message_id=?", (message_id,))
                self._db.execute(
                    "DELETE FROM outbox WHERE peer_id=? AND message_id=?", (peer_id, message_id)
                )
                self._db.execute("COMMIT")
                return kind
            except Exception:
                self._db.execute("ROLLBACK")
                raise

    def insert_inbox(self, peer_id: int, message_id: str, kind: str,
                     envelope: str, received_at: float) -> bool:
        with self._lock:
            cursor = self._db.execute(
                """INSERT OR IGNORE INTO inbox(peer_id,message_id,kind,envelope,received_at)
                   VALUES(?,?,?,?,?)""",
                (peer_id, message_id, kind, envelope, received_at),
            )
            return cursor.rowcount == 1

    def due_inbox(self, now: float, limit: int = 64):
        with self._lock:
            return self._db.execute(
                """SELECT peer_id,message_id,kind,envelope,received_at,dispatch_attempts
                   FROM inbox WHERE processed=0 AND next_dispatch<=?
                   ORDER BY received_at LIMIT ?""",
                (now, limit),
            ).fetchall()

    def retry_inbox_later(self, peer_id: int, message_id: str, attempts: int, when: float):
        with self._lock:
            self._db.execute(
                """UPDATE inbox SET dispatch_attempts=?,next_dispatch=?
                   WHERE peer_id=? AND message_id=?""",
                (attempts, when, peer_id, message_id),
            )

    def complete_inbox(self, peer_id: int, message_id: str):
        with self._lock:
            self._db.execute(
                "UPDATE inbox SET processed=1 WHERE peer_id=? AND message_id=?",
                (peer_id, message_id),
            )

    def store_response(self, request_id: str, response_json: str):
        with self._lock:
            self._db.execute(
                "UPDATE requests SET response_json=? WHERE message_id=?",
                (response_json, request_id),
            )

    def get_request(self, request_id: str):
        with self._lock:
            return self._db.execute(
                "SELECT peer_id,accepted,response_json FROM requests WHERE message_id=?",
                (request_id,),
            ).fetchone()

    def pending_requests(self):
        with self._lock:
            return self._db.execute(
                "SELECT message_id,peer_id,accepted,response_json FROM requests WHERE response_json IS NULL"
            ).fetchall()

    def close(self):
        with self._lock:
            self._db.close()
