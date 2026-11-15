/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "Importers/Constructor/ObjectImporter.h"

class INiagaraScriptImporter final : public IObjectImporter {
public:
	virtual bool Import() override;
	virtual void Repair(UObject* Asset) const override;

	static bool RestoreSource(UObject* Script, FUObjectExportContainer* Container);

	virtual void Validate(UObject* Asset) const override;
};

REGISTER_IMPORTER(INiagaraScriptImporter, TArray<FString>{
	TEXT("NiagaraScript")
}, TEXT("Niagara Assets"));
