#include "scoredb.h"
#include "sqlite3.h"

ScoreDb::~ScoreDb()
{
	if (m_db)
		sqlite3_close(m_db);
}

bool ScoreDb::open(const std::string& path)
{
	if (sqlite3_open(path.c_str(), &m_db) != SQLITE_OK)
	{
		m_error = m_db ? sqlite3_errmsg(m_db) : "no se pudo abrir la base";
		if (m_db) { sqlite3_close(m_db); m_db = nullptr; }
		return false;
	}

	const char* ddl =
		"CREATE TABLE IF NOT EXISTS scores ("
		"  id        INTEGER PRIMARY KEY AUTOINCREMENT,"
		"  name      TEXT    NOT NULL,"
		"  score     INTEGER NOT NULL,"
		"  played_at TEXT    NOT NULL DEFAULT (datetime('now'))"
		");"
		// El menu ordena por puntuacion y, a igualdad, por antiguedad.
		"CREATE INDEX IF NOT EXISTS idx_scores_rank ON scores(score DESC, id ASC);";

	char* err = nullptr;
	if (sqlite3_exec(m_db, ddl, nullptr, nullptr, &err) != SQLITE_OK)
	{
		m_error = err ? err : "no se pudo crear la tabla";
		sqlite3_free(err);
		sqlite3_close(m_db);
		m_db = nullptr;
		return false;
	}
	return true;
}

bool ScoreDb::addScore(const std::string& name, int score)
{
	if (!m_db)
		return false;

	// Sentencia preparada con parametros: el nombre lo escribe el jugador y no
	// debe concatenarse nunca dentro del SQL.
	sqlite3_stmt* st = nullptr;
	const char* sql = "INSERT INTO scores(name, score) VALUES(?, ?);";
	if (sqlite3_prepare_v2(m_db, sql, -1, &st, nullptr) != SQLITE_OK)
	{
		m_error = sqlite3_errmsg(m_db);
		return false;
	}
	sqlite3_bind_text(st, 1, name.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_int(st, 2, score);

	bool ok = (sqlite3_step(st) == SQLITE_DONE);
	if (!ok)
		m_error = sqlite3_errmsg(m_db);
	sqlite3_finalize(st);
	return ok;
}

std::vector<ScoreEntry> ScoreDb::topScores(int limit) const
{
	std::vector<ScoreEntry> out;
	if (!m_db)
		return out;

	sqlite3_stmt* st = nullptr;
	const char* sql =
		"SELECT name, score, played_at FROM scores "
		"ORDER BY score DESC, id ASC LIMIT ?;";
	if (sqlite3_prepare_v2(m_db, sql, -1, &st, nullptr) != SQLITE_OK)
	{
		m_error = sqlite3_errmsg(m_db);
		return out;
	}
	sqlite3_bind_int(st, 1, limit);

	while (sqlite3_step(st) == SQLITE_ROW)
	{
		ScoreEntry e;
		const unsigned char* n = sqlite3_column_text(st, 0);
		e.name = n ? reinterpret_cast<const char*>(n) : "";
		e.score = sqlite3_column_int(st, 1);
		const unsigned char* d = sqlite3_column_text(st, 2);
		e.playedAt = d ? reinterpret_cast<const char*>(d) : "";
		out.push_back(e);
	}
	sqlite3_finalize(st);
	return out;
}

int ScoreDb::bestScore() const
{
	if (!m_db)
		return 0;
	sqlite3_stmt* st = nullptr;
	if (sqlite3_prepare_v2(m_db, "SELECT COALESCE(MAX(score), 0) FROM scores;",
	                       -1, &st, nullptr) != SQLITE_OK)
		return 0;
	int best = 0;
	if (sqlite3_step(st) == SQLITE_ROW)
		best = sqlite3_column_int(st, 0);
	sqlite3_finalize(st);
	return best;
}

int ScoreDb::scoreCount() const
{
	if (!m_db)
		return 0;
	sqlite3_stmt* st = nullptr;
	if (sqlite3_prepare_v2(m_db, "SELECT COUNT(*) FROM scores;",
	                       -1, &st, nullptr) != SQLITE_OK)
		return 0;
	int n = 0;
	if (sqlite3_step(st) == SQLITE_ROW)
		n = sqlite3_column_int(st, 0);
	sqlite3_finalize(st);
	return n;
}

bool ScoreDb::clearAll()
{
	if (!m_db)
		return false;
	char* err = nullptr;
	// Se reinicia tambien el contador de AUTOINCREMENT.
	const char* sql = "DELETE FROM scores;"
	                  "DELETE FROM sqlite_sequence WHERE name='scores';";
	if (sqlite3_exec(m_db, sql, nullptr, nullptr, &err) != SQLITE_OK)
	{
		m_error = err ? err : "no se pudo borrar";
		sqlite3_free(err);
		return false;
	}
	return true;
}
