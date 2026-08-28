// Almacen de puntuaciones sobre SQLite.
//
// Se usa la amalgamacion oficial incrustada en third_party/sqlite, asi que no
// hace falta instalar nada ni en Windows ni en Raspberry Pi, y ambas plataformas
// compilan exactamente el mismo codigo.
#ifndef SCOREDB_H
#define SCOREDB_H

#include <string>
#include <vector>

struct ScoreEntry
{
	std::string name;
	int score = 0;
	std::string playedAt;   // fecha en UTC, "YYYY-MM-DD HH:MM:SS"
};

class ScoreDb
{
public:
	~ScoreDb();

	// Abre (o crea) la base y su tabla. Devuelve false si no se pudo, en cuyo
	// caso el resto de metodos son inocuos y el juego sigue funcionando sin
	// guardar nada.
	bool open(const std::string& path);
	bool isOpen() const { return m_db != nullptr; }
	const std::string& lastError() const { return m_error; }

	bool addScore(const std::string& name, int score);
	std::vector<ScoreEntry> topScores(int limit) const;
	int bestScore() const;
	int scoreCount() const;
	bool clearAll();

private:
	struct sqlite3* m_db = nullptr;
	mutable std::string m_error;
};

#endif
