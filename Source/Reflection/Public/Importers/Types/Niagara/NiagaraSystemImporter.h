/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "Importers/Constructor/ObjectImporter.h"

class INiagaraSystemImporter final : public IObjectImporter {
public:
	virtual bool Import() override;
	virtual void Repair(UObject* Asset) const override;
};

REGISTER_IMPORTER(INiagaraSystemImporter, TArray<FString>{
	TEXT("NiagaraSystem")
}, TEXT("Niagara Assets"));
