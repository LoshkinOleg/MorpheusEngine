import sqlite3 from "sqlite3";
import { open } from "sqlite";

export type GameDb = Awaited<ReturnType<typeof open<sqlite3.Database, sqlite3.Statement>>>;
