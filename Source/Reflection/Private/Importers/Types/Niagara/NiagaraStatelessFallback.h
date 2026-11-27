/* Copyright Reflection Contributors 2024-2026 */

#pragma once

struct FUObjectExportContainer;
class FJsonObject;
class UNiagaraSystem;
class UObjectSerializer;

namespace FNiagaraStatelessFallback {
	bool Convert(UNiagaraSystem& System, const FJsonObject& Properties, FUObjectExportContainer& Exports,
		UObjectSerializer& Serializer, bool& bConverted);
}
