/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "Importers/Constructor/ObjectImporter.h"

class UNiagaraSystem;

class INiagaraSystemImporter final : public IObjectImporter {
public:
	virtual bool Import() override;

	static bool RestorePreviewPlaybackRange(UNiagaraSystem& System);

private:
	bool RestoreSystem(UNiagaraSystem* System) const;
};

REGISTER_IMPORTER(INiagaraSystemImporter, TArray<FString>{
	TEXT("NiagaraSystem")
}, TEXT("Niagara Assets"));
