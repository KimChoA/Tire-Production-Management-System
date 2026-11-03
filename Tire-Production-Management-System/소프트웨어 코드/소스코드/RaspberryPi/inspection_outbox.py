"""Durable exactly-once *database effect* delivery of inspection messages.

The Pi may send one message more than once after a lost ACK. The central server
must have a unique event_id and return INSPECTION_ACK only after SQLite commit.
"""
from __future__ import annotations
import json
import os
import socket
import sqlite3
import threading
import time
import uuid
from pathlib import Path


class InspectionOutbox:
    def __init__(self, database_path, host, port, token, *, retry_seconds=1.0):
        if len(token) < 16:
            raise ValueError('INSPECTION_INGEST_TOKEN은 16자 이상으로 설정해야 합니다.')
        self.path = Path(database_path).expanduser().resolve()
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.host, self.port, self.token = host, int(port), token
        self.retry_seconds = retry_seconds
        self._lock = threading.RLock()
        self._wake = threading.Event()
        self._closed = threading.Event()
        self._db = sqlite3.connect(str(self.path), check_same_thread=False, timeout=10)
        self._db.execute('PRAGMA journal_mode=WAL')
        self._db.execute('PRAGMA synchronous=FULL')
        self._db.execute('''CREATE TABLE IF NOT EXISTS pending_inspection (
            event_id TEXT PRIMARY KEY,
            payload TEXT NOT NULL,
            created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
            attempts INTEGER NOT NULL DEFAULT 0,
            last_error TEXT NOT NULL DEFAULT ''
        )''')
        self._db.commit()

    def enqueue(self, result):
        if result.get('is_demo') is not False:
            raise ValueError('실검사 전송은 is_demo=false가 필수입니다.')
        if result.get('result') not in ('OK', 'NG'):
            raise ValueError('실검사 결과는 OK/NG만 저장합니다.')
        if not str(result.get('tire_id', '')).startswith('TIRE-'):
            raise ValueError('실검사 tire_id 형식 오류')
        event_id = str(result.get('event_id') or uuid.uuid4())
        uuid.UUID(event_id)
        packet = {'type': 'INSPECTION', **result,
                  'event_id': event_id, 'data_source': 'HARDWARE',
                  'is_demo': False}
        # Authorization lives on the wire, not in the persistent queue database.
        payload = json.dumps(packet, ensure_ascii=False, separators=(',', ':'))
        with self._lock:
            self._db.execute('INSERT INTO pending_inspection(event_id,payload) VALUES(?,?)', (event_id, payload))
            self._db.commit()
        self._wake.set()
        return event_id

    def pending_count(self):
        with self._lock:
            return self._db.execute('SELECT COUNT(*) FROM pending_inspection').fetchone()[0]

    def _next(self):
        with self._lock:
            return self._db.execute('SELECT event_id,payload,attempts FROM pending_inspection ORDER BY rowid LIMIT 1').fetchone()

    def send_once(self):
        item = self._next()
        if item is None:
            return False
        event_id, payload, attempts = item
        data = json.loads(payload)
        data['ingest_token'] = self.token
        try:
            with socket.create_connection((self.host, self.port), timeout=3) as conn:
                conn.settimeout(4)
                conn.sendall((json.dumps(data, ensure_ascii=False) + '\n').encode('utf-8'))
                buf = bytearray()
                while not buf.endswith(b'\n'):
                    chunk = conn.recv(4096)
                    if not chunk:
                        raise ConnectionError('저장 ACK 전에 서버 연결이 종료됐습니다.')
                    buf.extend(chunk)
                    if len(buf) > 65536:
                        raise RuntimeError('서버 응답 길이가 비정상적입니다.')
                ack = json.loads(buf.decode('utf-8'))
            if (ack.get('type') != 'INSPECTION_ACK' or
                ack.get('status') != 'OK' or
                ack.get('event_id') != event_id or
                ack.get('outcome') not in ('SAVED', 'DUPLICATE')):
                raise RuntimeError('저장 확인 거부: '+repr(ack))
            with self._lock:
                self._db.execute('DELETE FROM pending_inspection WHERE event_id=?',(event_id,))
                self._db.commit()
            print('[INSPECTION ACK]', event_id, ack['outcome'], flush=True)
            return True
        except (OSError, ValueError, RuntimeError, json.JSONDecodeError) as exc:
            with self._lock:
                self._db.execute('UPDATE pending_inspection SET attempts=?, last_error=? WHERE event_id=?',
                                 (attempts+1, str(exc)[:500], event_id))
                self._db.commit()
            print('[INSPECTION RETRY]', event_id, str(exc), flush=True)
            return False

    def run(self):
        while not self._closed.is_set():
            if not self.send_once():
                self._wake.wait(self.retry_seconds)
                self._wake.clear()

    def start(self):
        thread = threading.Thread(target=self.run, daemon=True, name='inspection-outbox')
        thread.start()
        return thread

    def close(self):
        self._closed.set()
        self._wake.set()
        with self._lock:
            self._db.close()
