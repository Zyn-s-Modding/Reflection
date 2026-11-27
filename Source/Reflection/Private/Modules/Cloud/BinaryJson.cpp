/* Copyright Reflection Contributors 2024-2026 */

#include "Modules/Cloud/BinaryJson.h"

#include "Containers/StringConv.h"

const TCHAR* FReflectionBinaryJson::ContentType = TEXT("application/x-reflection-binary");
const TCHAR* FReflectionBinaryJson::AcceptHeader = TEXT("application/x-reflection-binary, application/json;q=0.9");

/* Named rather than anonymous, so a unity build cannot fold these into another file's helpers */
namespace ReflectionBinaryJsonFormat {
	/* Kept in step with Mirror's Objects/BinaryJson.cs */
	constexpr uint8 Version = 1;

	constexpr uint8 TagNull = 0x00;
	constexpr uint8 TagFalse = 0x01;
	constexpr uint8 TagTrue = 0x02;
	constexpr uint8 TagInteger = 0x03;
	constexpr uint8 TagDouble = 0x04;
	constexpr uint8 TagString = 0x05;
	constexpr uint8 TagStringDef = 0x06;
	constexpr uint8 TagStringRef = 0x07;
	constexpr uint8 TagArray = 0x08;
	constexpr uint8 TagObject = 0x09;
	constexpr uint8 TagNumberText = 0x0A;
	constexpr uint8 TagEnd = 0xFF;

	/* UTF-8 off the wire, with the common case of plain ASCII widened directly rather than run
	 * through the general converter */
	FString ToString(const uint8* Bytes, const int32 Length) {
		if (Length <= 0) {
			return FString();
		}

		bool bAscii = true;

		for (int32 Index = 0; Index < Length; ++Index) {
			if (Bytes[Index] & 0x80) {
				bAscii = false;

				break;
			}
		}

		if (bAscii) {
			FString Out;

			TArray<TCHAR>& Characters = Out.GetCharArray();
			Characters.SetNumUninitialized(Length + 1);

			TCHAR* Write = Characters.GetData();

			for (int32 Index = 0; Index < Length; ++Index) {
				Write[Index] = static_cast<TCHAR>(Bytes[Index]);
			}

			Write[Length] = TEXT('\0');

			return Out;
		}

		const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Bytes), Length);

		return FString(Converted.Length(), Converted.Get());
	}

	struct FReader {
		const uint8* Cursor;
		const uint8* End;

		bool Byte(uint8& Out) {
			if (Cursor >= End) return false;

			Out = *Cursor++;

			return true;
		}

		bool Peek(uint8& Out) const {
			if (Cursor >= End) return false;

			Out = *Cursor;

			return true;
		}

		bool VarUInt(uint64& Out) {
			if (Cursor == End) return false;

			const uint8 First = *Cursor++;
			if (First < 0x80) {
				Out = First;
				return true;
			}

			uint64 Result = First & 0x7F;
			for (int32 Shift = 7; Shift < 64 && Cursor < End; Shift += 7) {
				const uint8 Next = *Cursor++;
				if (Shift == 63 && Next > 1) return false;

				Result |= static_cast<uint64>(Next & 0x7F) << Shift;

				if ((Next & 0x80) == 0) {
					Out = Result;

					return true;
				}
			}

			return false;
		}

		bool Double(double& Out) {
			if (End - Cursor < 8) return false;

			/* Little endian on the wire, as every platform the editor runs on is */
			FMemory::Memcpy(&Out, Cursor, sizeof(double));
			Cursor += 8;

			return true;
		}

		bool String(FString& Out) {
			uint64 Length = 0;

			if (!VarUInt(Length) || Length > static_cast<uint64>(End - Cursor) || Length > static_cast<uint64>(MAX_int32 - 1)) {
				return false;
			}

			Out = ToString(Cursor, static_cast<int32>(Length));
			Cursor += Length;

			return true;
		}
	};

	/* A container still being read. Only attached to whatever holds it once it is whole, so an
	 * array can be handed over by moving rather than by copying it. */
	struct FDecodeFrame {
		TSharedPtr<FJsonObject> Object;
		TArray<TSharedPtr<FJsonValue>> Items;
		int32 Key = INDEX_NONE;

		bool IsObject() const { return Object.IsValid(); }
	};
}

bool FReflectionBinaryJson::IsContentType(const FString& InContentType) {
	return InContentType.StartsWith(ContentType, ESearchCase::IgnoreCase);
}

TSharedPtr<FJsonValue> FReflectionBinaryJson::Decode(const uint8* Data, const int64 Size) {
	using namespace ReflectionBinaryJsonFormat;

	if (Data == nullptr || Size < 5 || Data[0] != 'R' || Data[1] != 'B' || Data[2] != 'J' || Data[3] != Version) {
		return nullptr;
	}

	FReader Reader { Data + 4, Data + Size };

	TArray<FString, TInlineAllocator<32>> Keys;

	/* A string said more than once is the same value every time it is said, and none of these is
	 * ever written to, so one of each serves the whole tree */
	TArray<TSharedPtr<FJsonValue>, TInlineAllocator<32>> Strings;

	const TSharedPtr<FJsonValue> Null = MakeShared<FJsonValueNull>();
	const TSharedPtr<FJsonValue> True = MakeShared<FJsonValueBoolean>(true);
	const TSharedPtr<FJsonValue> False = MakeShared<FJsonValueBoolean>(false);

	/* Walked with a stack of its own rather than by recursion, so a deep tree cannot run a worker
	 * thread's stack out. Frames are reused rather than popped, which keeps the stack from being
	 * shrunk and grown again at every container. */
	TArray<FDecodeFrame, TInlineAllocator<16>> Stack;
	Stack.SetNum(16);
	int32 Depth = 0;

	TSharedPtr<FJsonValue> Root;

	const auto Attach = [&](TSharedPtr<FJsonValue> Value) {
		if (Depth == 0) {
			Root = MoveTemp(Value);

			return;
		}

		FDecodeFrame& Top = Stack[Depth - 1];

		if (Top.IsObject()) {
			Top.Object->Values.Add(Keys[Top.Key], MoveTemp(Value));
			Top.Key = INDEX_NONE;

			return;
		}

		Top.Items.Add(MoveTemp(Value));
	};

	const auto Push = [&](const bool bObject) {
		if (Depth == Stack.Num()) {
			Stack.AddDefaulted(Stack.Num());
		}

		FDecodeFrame& Frame = Stack[Depth++];

		Frame.Object = bObject ? TSharedPtr<FJsonObject>(MakeShared<FJsonObject>()) : TSharedPtr<FJsonObject>();
		Frame.Items.Reset();
		Frame.Key = INDEX_NONE;
	};

	for (;;) {
		/* Inside a container, see whether it ends here, and for an object read the key first */
		if (Depth > 0) {
			FDecodeFrame& Top = Stack[Depth - 1];

			if (Top.IsObject()) {
				uint64 Key = 0;

				if (!Reader.VarUInt(Key)) return nullptr;

				if (Key == 0) {
					TSharedPtr<FJsonValue> Whole = MakeShared<FJsonValueObject>(MoveTemp(Top.Object));
					Top.Object.Reset();
					Depth--;

					Attach(MoveTemp(Whole));
					if (Depth == 0) break;

					continue;
				}

				if (Key == 1) {
					FString Named;

					if (!Reader.String(Named)) return nullptr;

					Top.Key = Keys.Add(MoveTemp(Named));
				} else {
					const uint64 Index = Key - 2;

					if (Index >= static_cast<uint64>(Keys.Num())) return nullptr;

					Top.Key = static_cast<int32>(Index);
				}
			} else {
				uint8 Next = 0;

				if (!Reader.Peek(Next)) return nullptr;

				if (Next == TagEnd) {
					Reader.Cursor++;

					TSharedPtr<FJsonValue> Whole = MakeShared<FJsonValueArray>(MoveTemp(Top.Items));
					Top.Items.Reset();
					Depth--;

					Attach(MoveTemp(Whole));
					if (Depth == 0) break;

					continue;
				}
			}
		}

		uint8 Tag = 0;

		if (!Reader.Byte(Tag)) return nullptr;

		switch (Tag) {
			case TagNull:
				Attach(Null);

				break;

			case TagFalse:
				Attach(False);

				break;

			case TagTrue:
				Attach(True);

				break;

			case TagInteger: {
				uint64 ZigZag = 0;

				if (!Reader.VarUInt(ZigZag)) return nullptr;

				const int64 Value = static_cast<int64>(ZigZag >> 1) ^ -static_cast<int64>(ZigZag & 1);

				Attach(MakeShared<FJsonValueNumber>(static_cast<double>(Value)));

				break;
			}

			case TagDouble: {
				double Value = 0.0;

				if (!Reader.Double(Value)) return nullptr;

				Attach(MakeShared<FJsonValueNumber>(Value));

				break;
			}

			case TagString: {
				FString Value;

				if (!Reader.String(Value)) return nullptr;

				Attach(MakeShared<FJsonValueString>(MoveTemp(Value)));

				break;
			}

			case TagStringDef: {
				FString Value;

				if (!Reader.String(Value)) return nullptr;

				TSharedPtr<FJsonValue> Shared = MakeShared<FJsonValueString>(MoveTemp(Value));
				Strings.Add(Shared);

				Attach(MoveTemp(Shared));

				break;
			}

			case TagStringRef: {
				uint64 Index = 0;

				if (!Reader.VarUInt(Index) || Index >= static_cast<uint64>(Strings.Num())) return nullptr;

				Attach(Strings[static_cast<int32>(Index)]);

				break;
			}

			case TagNumberText: {
				/* Too large for a double to carry exactly, so read the way the json reader would have */
				FString Value;

				if (!Reader.String(Value)) return nullptr;

				Attach(MakeShared<FJsonValueNumber>(FCString::Atod(*Value)));

				break;
			}

			case TagArray:
				Push(false);

				break;

			case TagObject:
				Push(true);

				break;

			default:
				return nullptr;
		}

		/* A document that is a single value rather than a container is done as soon as it is read */
		if (Depth == 0) break;
	}

	return Reader.Cursor == Reader.End ? Root : nullptr;
}

TSharedPtr<FJsonObject> FReflectionBinaryJson::DecodeObject(const uint8* Data, const int64 Size) {
	const TSharedPtr<FJsonValue> Value = Decode(Data, Size);

	if (!Value.IsValid() || Value->Type != EJson::Object) {
		return nullptr;
	}

	return Value->AsObject();
}
