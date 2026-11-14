#include "TacticalMap.h"
#include "TacticalMap.Canvas.h"
#include "TacticalMap.GdiText.h"

#include <Phobos.h>

#include <CellClass.h>
#include <Drawing.h>
#include <EventClass.h>
#include <HouseClass.h>
#include <InputManagerClass.h>
#include <MapClass.h>
#include <MouseClass.h>
#include <RadarClass.h>
#include <SessionClass.h>
#include <Surface.h>
#include <TacticalClass.h>

#include <CCINIClass.h>
#include <Unsorted.h>
#include <Utilities/Debug.h>
#include <Utilities/GeneralUtils.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <imm.h>

#include <Utilities/Macro.h>

TacticalMap::SettingsStruct TacticalMap::Settings {};
bool TacticalMap::Opened = false;

namespace
{
	constexpr int MinSourceSide = 8;
	constexpr int MinDestSide = 16;

	bool RadarToCell(const RectangleStruct& src, int radarX, int radarY, int& cellX, int& cellY);

	// Input state shared by the two click paths.
	int LastHandledFrame = -1;
	int ClickLogCount = 0;
	int MoveLogCount = 0;
	int RadarLogCount = 0;
	bool GadgetBlockLogged = false; // former diagnostic, kept for layout stability

	// Camera pin: the engine's edge scrolling moves the camera before the frame is
	// rendered, so it is restored to this position at the start of every render.
	Point2D PinnedCamera { 0, 0 };
	bool PinValid = false;

	// A click is queued by the input path and forwarded to the engine's own radar
	// click handler at render time, outside of gadget input processing.
	Point2D PendingClick { 0, 0 };
	bool HasPendingClick = false;

	// --- annotation tools -----------------------------------------------------

	enum class Tool { Camera, Brush, Eraser, Ruler, Text, Marker };
	enum class ToolButton { Camera, Brush, Eraser, Ruler, Text, Marker, Color, Width, Clear, Count };

	struct MeasureLine
	{
		Point2D A { 0, 0 }; // normalised map coordinates (0..1 of the minimap rect)
		Point2D B { 0, 0 };
		uint32_t Id = 0;
		mutable GdiTextBitmap Label;
		mutable int LabelTenths = -1;
	};

	struct MarkerItem
	{
		Point2D Pos { 0, 0 };
		unsigned Rgb = 0xFFFFFF;
		int Size = 12;
		uint32_t Id = 0;
	};

	struct TextItem
	{
		Point2D Pos { 0, 0 };       // normalised map coordinates
		std::wstring Value;
		unsigned Rgb = 0xFFFFFF;
		int Size = 14;
		uint32_t Id = 0;
		GdiTextBitmap Bitmap;        // rendered once at commit time
		mutable RectangleStruct Bounds {}; // last on-screen bounds (for click-to-delete)
	};

	enum class UndoKind { Canvas, TextAdd, TextDelete, MarkerAdd, MarkerDelete, MeasureAdd, MeasureDelete, Clear };

	struct UndoAction
	{
		UndoKind Kind = UndoKind::Canvas;
		size_t Bytes = 0;
		uint32_t OpId = 0;
		std::vector<std::pair<int, DWORD>> Pixels;
		TextItem Text {};
		int TextIndex = -1;
		MarkerItem Marker {};
		int MarkerIndex = -1;
		MeasureLine Measure {};
		int MeasureIndex = -1;
		std::vector<DWORD> CanvasSnapshot;
		std::vector<TextItem> TextsSnapshot;
		std::vector<MarkerItem> MarkersSnapshot;
		std::vector<MeasureLine> MeasuresSnapshot;
	};

	std::vector<UndoAction> UndoHistory;
	size_t UndoBytes = 0;

	void PushUndo(UndoAction&& action);

	// --- multiplayer sharing -------------------------------------------------
	// Annotations travel through the engine's own event queue as a private event
	// type, so they follow exactly the same reliable, frame-ordered path as
	// orders. Nothing in here writes game state, so the simulation stays in sync.
	uint32_t NextOpId = 1;
	uint32_t CurrentStrokeOp = 0;
	int LastStrokeSendFrame = -1;
	int LastStrokeSendX = -1;
	int LastStrokeSendY = -1;

	uint32_t NetNewOpId()
	{
		const unsigned house = HouseClass::CurrentPlayer
			? (static_cast<unsigned>(HouseClass::CurrentPlayer->ArrayIndex) & 0xFF)
			: 0u;

		return (static_cast<uint32_t>(house) << 24) | (NextOpId++ & 0xFFFFFF);
	}

	void NetSendStrokeBegin(int rgb, int radius, bool erase, int x, int y);
	void NetSendStrokePoint(int x, int y);
	void NetSendStrokeEnd();
	void NetSendTextAdd(const TextItem& item);
	void NetSendTextDelete(uint32_t opId, uint32_t targetId);
	void NetSendMarkerAdd(const MarkerItem& marker);
	void NetSendMarkerDelete(uint32_t opId, uint32_t targetId);
	void NetSendMeasureAdd(const MeasureLine& line);
	void NetSendMeasureDelete(uint32_t opId, uint32_t targetId);
	void NetSendClear(uint32_t opId);
	void NetSendUndo(uint32_t targetId);


	Tool CurrentTool = Tool::Camera;
	unsigned BrushRgb = 0xFFF050;
	int BrushWidth = 2;
	int ColorIndex = 1;
	int WidthIndex = 0;
	bool StrokeActive = false;
	bool MeasureActive = false;
	Point2D MeasureStart { 0, 0 };
	Point2D MeasureEnd { 0, 0 };
	std::vector<MeasureLine> Measures;
	bool PrevLeftDown = false;
	bool PrevRightDown = false;

	std::vector<TextItem> Texts;
	std::vector<MarkerItem> Markers;

	bool EditingText = false;
	std::wstring EditValue;
	std::wstring EditComposition;
	Point2D EditPos { 0, 0 };
	unsigned EditRgb = 0xFFFFFF;
	int EditSize = 14;
	bool ImeComposing = false;

	HWND GameWindow = nullptr;
	WNDPROC OldWndProc = nullptr;
	bool WndProcInstalled = false;

	const int TextSizes[] = { 12, 14, 16, 18, 20, 22, 26 };

	const unsigned Palette[] = {
		0xFFFFFF, 0xFFF050, 0xFF4040, 0x40FF60,
		0x40C0FF, 0xFF60FF, 0x202020, 0x808080
	};

	const int Widths[] = { 2, 4, 6, 8, 10, 12, 16 };

	// ===================== multiplayer annotation sharing =====================
	//
	// The overlay is client-side only, so annotations must not go through the
	// regular command stream. They are sent as a private EventClass type (0x41,
	// in the range Ares/CnCNet leave free). The engine queues the event, ships it
	// to every peer and executes it at the scheduled frame, which gives us the
	// same ordering and reliability as orders without touching game state.
	//
	// Every peer executes the event (so command counts stay identical); only the
	// handler decides whether to display it (allies only, never the sender).

	constexpr BYTE NetEventType = 0x41;
	constexpr size_t NetPayloadSize = 64; // bytes of DataBuffer transmitted
	constexpr size_t NetHeaderSize = 10;
	constexpr size_t NetChunkSize = NetPayloadSize - NetHeaderSize;
	constexpr BYTE NetVersion = 1;

	enum class NetKind : BYTE
	{
		StrokeBegin = 1,
		StrokePoint = 2,
		StrokeEnd = 3,
		TextAdd = 4,
		TextDelete = 5,
		MarkerAdd = 6,
		MarkerDelete = 7,
		MeasureAdd = 8,
		MeasureDelete = 9,
		Clear = 10,
		Undo = 11
	};

	struct RemoteOp
	{
		uint32_t Id = 0;
		BYTE Kind = 0;
		TextItem Text {};
		MarkerItem Marker {};
		MeasureLine Measure {};
		std::vector<std::pair<int, DWORD>> Stroke;
		std::vector<DWORD> Canvas;
		std::vector<TextItem> Texts;
		std::vector<MarkerItem> Markers;
		std::vector<MeasureLine> Measures;
	};

	struct NetFragment
	{
		std::vector<BYTE> Data;
		std::vector<BYTE> Got;
		int Total = 0;
		int Count = 0;
	};

	std::vector<RemoteOp> RemoteOps;
	std::map<uint32_t, NetFragment> NetFragments;
	uint32_t RemoteStrokeOp = 0;
	bool RemoteStrokeActive = false;
	bool RemoteStrokeIgnored = false;
	int NetLogCount = 0;

	bool NetEnabled()
	{
		return TacticalMap::Settings.ShareWithTeam
			&& SessionClass::IsMultiplayer()
			&& HouseClass::CurrentPlayer != nullptr;
	}

	inline uint32_t ReadU32(const BYTE* p)
	{
		return static_cast<uint32_t>(p[0])
			| (static_cast<uint32_t>(p[1]) << 8)
			| (static_cast<uint32_t>(p[2]) << 16)
			| (static_cast<uint32_t>(p[3]) << 24);
	}

	inline void AppendU16(std::vector<BYTE>& out, int value)
	{
		const uint16_t v = static_cast<uint16_t>(std::clamp(value, 0, 65535));
		out.push_back(static_cast<BYTE>(v & 0xFF));
		out.push_back(static_cast<BYTE>(v >> 8));
	}

	inline void AppendU32(std::vector<BYTE>& out, uint32_t value)
	{
		out.push_back(static_cast<BYTE>(value & 0xFF));
		out.push_back(static_cast<BYTE>((value >> 8) & 0xFF));
		out.push_back(static_cast<BYTE>((value >> 16) & 0xFF));
		out.push_back(static_cast<BYTE>((value >> 24) & 0xFF));
	}

	std::string WideToUtf8(const std::wstring& text)
	{
		if (text.empty())
			return {};

		const int needed = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
			nullptr, 0, nullptr, nullptr);

		if (needed <= 0)
			return {};

		std::string out(static_cast<size_t>(needed), '\0');
		WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
			out.data(), needed, nullptr, nullptr);
		return out;
	}

	std::wstring Utf8ToWide(const BYTE* data, size_t size)
	{
		if (!data || !size)
			return {};

		const int needed = MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<const char*>(data),
			static_cast<int>(size), nullptr, 0);

		if (needed <= 0)
			return {};

		std::wstring out(static_cast<size_t>(needed), L'\0');
		MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<const char*>(data),
			static_cast<int>(size), out.data(), needed);
		return out;
	}

	void SendNetPacket(NetKind kind, uint32_t opId, const std::vector<BYTE>& payload)
	{
		if (!NetEnabled())
			return;

		const size_t total = payload.empty()
			? 1
			: (payload.size() + NetChunkSize - 1) / NetChunkSize;

		if (total > 255)
			return;

		const int house = HouseClass::CurrentPlayer->ArrayIndex;

		for (size_t i = 0; i < total; ++i)
		{
			EventClass event(house, EventType::Empty);
			event.Type = static_cast<EventType>(NetEventType);
			event.IsExecuted = false;
			event.HouseIndex = static_cast<char>(house);
			std::memset(event.DataBuffer, 0, sizeof(event.DataBuffer));

			BYTE* data = reinterpret_cast<BYTE*>(event.DataBuffer);
			data[0] = 'T';
			data[1] = 'M';
			data[2] = NetVersion;
			data[3] = static_cast<BYTE>(kind);
			data[4] = static_cast<BYTE>(opId & 0xFF);
			data[5] = static_cast<BYTE>((opId >> 8) & 0xFF);
			data[6] = static_cast<BYTE>((opId >> 16) & 0xFF);
			data[7] = static_cast<BYTE>((opId >> 24) & 0xFF);
			data[8] = static_cast<BYTE>(i);
			data[9] = static_cast<BYTE>(total);

			const size_t offset = i * NetChunkSize;
			const size_t count = payload.empty()
				? 0
				: std::min(NetChunkSize, payload.size() - offset);

			if (count)
				std::memcpy(data + NetHeaderSize, payload.data() + offset, count);

			if (!EventClass::OutList.Add(event) && NetLogCount < 30)
			{
				++NetLogCount;
				Debug::Log("[Phobos::TacticalMap] net send queue full, kind=%d dropped\n",
					static_cast<int>(kind));
			}
		}
	}

	void RememberRemoteOp(RemoteOp&& op)
	{
		// A clear keeps a full canvas snapshot (a few MB); only the most recent one
		// can be undone remotely, so drop older ones.
		if (op.Kind == static_cast<BYTE>(NetKind::Clear))
		{
			RemoteOps.erase(std::remove_if(RemoteOps.begin(), RemoteOps.end(),
				[](const RemoteOp& entry) { return entry.Kind == static_cast<BYTE>(NetKind::Clear); }),
				RemoteOps.end());
		}

		// Keep enough history for the originator's undo without unbounded growth.
		while (RemoteOps.size() >= 64)
			RemoteOps.erase(RemoteOps.begin());

		RemoteOps.push_back(std::move(op));
	}

	int FindTextIndex(uint32_t id)
	{
		for (int i = 0; i < static_cast<int>(Texts.size()); ++i)
		{
			if (Texts[i].Id == id)
				return i;
		}

		return -1;
	}

	int FindMarkerIndex(uint32_t id)
	{
		for (int i = 0; i < static_cast<int>(Markers.size()); ++i)
		{
			if (Markers[i].Id == id)
				return i;
		}

		return -1;
	}

	int FindMeasureIndex(uint32_t id)
	{
		for (int i = 0; i < static_cast<int>(Measures.size()); ++i)
		{
			if (Measures[i].Id == id)
				return i;
		}

		return -1;
	}

	void ApplyRemoteUndo(uint32_t targetId)
	{
		for (auto it = RemoteOps.begin(); it != RemoteOps.end(); ++it)
		{
			if (it->Id != targetId)
				continue;

			switch (static_cast<NetKind>(it->Kind))
			{
			case NetKind::StrokeBegin:
				TacticalMapCanvas::ApplyEdits(it->Stroke);
				break;

			case NetKind::TextAdd:
			{
				const int index = FindTextIndex(it->Id);
				if (index >= 0)
					Texts.erase(Texts.begin() + index);
				break;
			}

			case NetKind::TextDelete:
				if (!it->Text.Value.empty())
					Texts.push_back(it->Text);
				break;

			case NetKind::MarkerAdd:
			{
				const int index = FindMarkerIndex(it->Id);
				if (index >= 0)
					Markers.erase(Markers.begin() + index);
				break;
			}

			case NetKind::MarkerDelete:
				if (it->Marker.Id)
					Markers.push_back(it->Marker);
				break;

			case NetKind::MeasureAdd:
			{
				const int index = FindMeasureIndex(it->Id);
				if (index >= 0)
					Measures.erase(Measures.begin() + index);
				break;
			}

			case NetKind::MeasureDelete:
				if (it->Measure.Id)
					Measures.push_back(it->Measure);
				break;

			case NetKind::Clear:
				if (!it->Canvas.empty())
					TacticalMapCanvas::Restore(it->Canvas);
				else
					TacticalMapCanvas::Reset();

				Texts = it->Texts;
				Markers = it->Markers;
				Measures = it->Measures;
				break;

			default:
				break;
			}

			RemoteOps.erase(it);
			return;
		}
	}

	void DispatchNetPacket(BYTE kind, uint32_t id, const BYTE* data, size_t size)
	{
		const auto need = [&](size_t count) { return size >= count; };

		if (NetLogCount < 30)
		{
			++NetLogCount;
			Debug::Log("[Phobos::TacticalMap] net recv kind=%d id=%08X bytes=%d\n",
				static_cast<int>(kind), id, static_cast<int>(size));
		}

		switch (static_cast<NetKind>(kind))
		{
		case NetKind::StrokeBegin:
		{
			if (!need(10))
				return;

			const int rgb = data[0] | (data[1] << 8) | (data[2] << 16);
			const int radius = data[3] | (data[4] << 8);
			const bool erase = data[5] != 0;
			const int x = data[6] | (data[7] << 8);
			const int y = data[8] | (data[9] << 8);

			if (StrokeActive)
			{
				// The local player is drawing right now: the canvas only supports
				// one stroke. Drop the remote one (cosmetic, next stroke syncs).
				RemoteStrokeIgnored = true;
				RemoteStrokeActive = false;
				return;
			}

			TacticalMapCanvas::BeginStroke(x, y, std::max(1, radius), rgb & 0xFFFFFF, erase);
			RemoteStrokeOp = id;
			RemoteStrokeActive = true;
			RemoteStrokeIgnored = false;
			break;
		}

		case NetKind::StrokePoint:
		{
			if (!RemoteStrokeActive || RemoteStrokeIgnored || !need(4))
				return;

			TacticalMapCanvas::ContinueStroke(data[0] | (data[1] << 8), data[2] | (data[3] << 8));
			break;
		}

		case NetKind::StrokeEnd:
		{
			if (!RemoteStrokeActive || RemoteStrokeIgnored)
			{
				RemoteStrokeActive = false;
				RemoteStrokeIgnored = false;
				return;
			}

			TacticalMapCanvas::EndStroke();
			RemoteStrokeActive = false;

			RemoteOp op;
			op.Id = RemoteStrokeOp;
			op.Kind = static_cast<BYTE>(NetKind::StrokeBegin);

			if (TacticalMapCanvas::TakeStrokeEdits(op.Stroke))
				RememberRemoteOp(std::move(op));

			break;
		}

		case NetKind::TextAdd:
		{
			if (!need(10))
				return;

			TextItem item;
			item.Pos = Point2D { data[0] | (data[1] << 8), data[2] | (data[3] << 8) };
			item.Rgb = data[4] | (data[5] << 8) | (data[6] << 16);
			item.Size = std::clamp(static_cast<int>(data[7]), 6, 96);
			const int length = data[8] | (data[9] << 8);

			if (length <= 0 || size < 10 + static_cast<size_t>(length))
				return;

			item.Value = Utf8ToWide(data + 10, static_cast<size_t>(length));

			if (item.Value.empty())
				return;

			item.Id = id;
			TacticalMapGdi::Render(item.Value, item.Size, item.Rgb, item.Bitmap);
			Texts.push_back(std::move(item));

			RemoteOp op;
			op.Id = id;
			op.Kind = kind;
			RememberRemoteOp(std::move(op));
			break;
		}

		case NetKind::TextDelete:
		{
			if (!need(4))
				return;

			RemoteOp op;
			op.Id = id;
			op.Kind = kind;

			const int index = FindTextIndex(ReadU32(data));

			if (index >= 0)
			{
				op.Text = Texts[index];
				Texts.erase(Texts.begin() + index);
			}

			RememberRemoteOp(std::move(op));
			break;
		}

		case NetKind::MarkerAdd:
		{
			if (!need(8))
				return;

			MarkerItem marker;
			marker.Pos = Point2D { data[0] | (data[1] << 8), data[2] | (data[3] << 8) };
			marker.Rgb = data[4] | (data[5] << 8) | (data[6] << 16);
			marker.Size = std::max(4, static_cast<int>(data[7]));
			marker.Id = id;
			Markers.push_back(marker);

			RemoteOp op;
			op.Id = id;
			op.Kind = kind;
			RememberRemoteOp(std::move(op));
			break;
		}

		case NetKind::MarkerDelete:
		{
			if (!need(4))
				return;

			RemoteOp op;
			op.Id = id;
			op.Kind = kind;

			const int index = FindMarkerIndex(ReadU32(data));

			if (index >= 0)
			{
				op.Marker = Markers[index];
				Markers.erase(Markers.begin() + index);
			}

			RememberRemoteOp(std::move(op));
			break;
		}

		case NetKind::MeasureAdd:
		{
			if (!need(8))
				return;

			MeasureLine line;
			line.A = Point2D { data[0] | (data[1] << 8), data[2] | (data[3] << 8) };
			line.B = Point2D { data[4] | (data[5] << 8), data[6] | (data[7] << 8) };
			line.Id = id;
			Measures.push_back(std::move(line));

			RemoteOp op;
			op.Id = id;
			op.Kind = kind;
			RememberRemoteOp(std::move(op));
			break;
		}

		case NetKind::MeasureDelete:
		{
			if (!need(4))
				return;

			RemoteOp op;
			op.Id = id;
			op.Kind = kind;

			const int index = FindMeasureIndex(ReadU32(data));

			if (index >= 0)
			{
				op.Measure = Measures[index];
				Measures.erase(Measures.begin() + index);
			}

			RememberRemoteOp(std::move(op));
			break;
		}

		case NetKind::Clear:
		{
			RemoteOp op;
			op.Id = id;
			op.Kind = kind;

			if (TacticalMapCanvas::HasContent())
				TacticalMapCanvas::Save(op.Canvas);

			op.Texts = Texts;
			op.Markers = Markers;
			op.Measures = Measures;
			RememberRemoteOp(std::move(op));

			TacticalMapCanvas::Reset();
			Texts.clear();
			Markers.clear();
			Measures.clear();
			break;
		}

		case NetKind::Undo:
			if (need(4))
				ApplyRemoteUndo(ReadU32(data));

			break;

		default:
			break;
		}
	}

	void ReceiveNetEvent(const EventClass& event)
	{
		if (!NetEnabled())
			return;

		HouseClass* self = HouseClass::CurrentPlayer;
		const BYTE* data = reinterpret_cast<const BYTE*>(event.DataBuffer);

		if (data[0] != 'T' || data[1] != 'M' || data[2] != NetVersion)
			return;

		const BYTE kind = data[3];
		const uint32_t id = ReadU32(data + 4);

		// The sender's house travels in the top byte of the operation id, so the
		// alliance filter does not depend on how the engine fills HouseIndex.
		const int senderIndex = static_cast<int>((id >> 24) & 0xFF);

		if (senderIndex < 0 || senderIndex >= HouseClass::Array.Count)
			return;

		HouseClass* sender = HouseClass::Array.GetItem(senderIndex);

		if (!sender || sender == self || !self->IsAlliedWith(sender))
			return;

		const int seq = data[8];
		const int total = data[9];

		if (total <= 0 || seq < 0 || seq >= total)
			return;

		if (total == 1)
		{
			DispatchNetPacket(kind, id, data + NetHeaderSize, NetChunkSize);
			return;
		}

		if (NetFragments.size() > 32)
			NetFragments.clear();

		NetFragment& fragment = NetFragments[id];

		if (fragment.Total != total)
		{
			fragment.Data.assign(static_cast<size_t>(total) * NetChunkSize, 0);
			fragment.Got.assign(static_cast<size_t>(total), 0);
			fragment.Total = total;
			fragment.Count = 0;
		}

		if (fragment.Data.size() < static_cast<size_t>(seq + 1) * NetChunkSize)
			return;

		std::memcpy(fragment.Data.data() + static_cast<size_t>(seq) * NetChunkSize,
			data + NetHeaderSize, NetChunkSize);

		if (!fragment.Got[seq])
		{
			fragment.Got[seq] = 1;
			++fragment.Count;
		}

		if (fragment.Count == fragment.Total)
		{
			std::vector<BYTE> payload = std::move(fragment.Data);
			NetFragments.erase(id);
			DispatchNetPacket(kind, id, payload.data(), payload.size());
		}
	}

	// --- senders -------------------------------------------------------------

	void NetSendStrokeBegin(int rgb, int radius, bool erase, int x, int y)
	{
		std::vector<BYTE> payload;
		payload.reserve(10);
		payload.push_back(static_cast<BYTE>(rgb & 0xFF));
		payload.push_back(static_cast<BYTE>((rgb >> 8) & 0xFF));
		payload.push_back(static_cast<BYTE>((rgb >> 16) & 0xFF));
		AppendU16(payload, radius);
		payload.push_back(erase ? 1 : 0);
		AppendU16(payload, x);
		AppendU16(payload, y);
		SendNetPacket(NetKind::StrokeBegin, CurrentStrokeOp, payload);
	}

	void NetSendStrokePoint(int x, int y)
	{
		std::vector<BYTE> payload;
		payload.reserve(4);
		AppendU16(payload, x);
		AppendU16(payload, y);
		SendNetPacket(NetKind::StrokePoint, CurrentStrokeOp, payload);
	}

	void NetSendStrokeEnd()
	{
		if (!CurrentStrokeOp)
			return;

		SendNetPacket(NetKind::StrokeEnd, CurrentStrokeOp, {});
	}

	void NetSendTextAdd(const TextItem& item)
	{
		const std::string text = WideToUtf8(item.Value);

		if (text.empty() || text.size() > 2000)
			return;

		std::vector<BYTE> payload;
		payload.reserve(10 + text.size());
		AppendU16(payload, item.Pos.X);
		AppendU16(payload, item.Pos.Y);
		payload.push_back(static_cast<BYTE>(item.Rgb & 0xFF));
		payload.push_back(static_cast<BYTE>((item.Rgb >> 8) & 0xFF));
		payload.push_back(static_cast<BYTE>((item.Rgb >> 16) & 0xFF));
		payload.push_back(static_cast<BYTE>(std::clamp(item.Size, 6, 96)));
		AppendU16(payload, static_cast<int>(text.size()));
		payload.insert(payload.end(), text.begin(), text.end());
		SendNetPacket(NetKind::TextAdd, item.Id, payload);
	}

	void NetSendTextDelete(uint32_t opId, uint32_t targetId)
	{
		std::vector<BYTE> payload;
		AppendU32(payload, targetId);
		SendNetPacket(NetKind::TextDelete, opId, payload);
	}

	void NetSendMarkerAdd(const MarkerItem& marker)
	{
		std::vector<BYTE> payload;
		payload.reserve(8);
		AppendU16(payload, marker.Pos.X);
		AppendU16(payload, marker.Pos.Y);
		payload.push_back(static_cast<BYTE>(marker.Rgb & 0xFF));
		payload.push_back(static_cast<BYTE>((marker.Rgb >> 8) & 0xFF));
		payload.push_back(static_cast<BYTE>((marker.Rgb >> 16) & 0xFF));
		payload.push_back(static_cast<BYTE>(std::clamp(marker.Size, 4, 255)));
		SendNetPacket(NetKind::MarkerAdd, marker.Id, payload);
	}

	void NetSendMarkerDelete(uint32_t opId, uint32_t targetId)
	{
		std::vector<BYTE> payload;
		AppendU32(payload, targetId);
		SendNetPacket(NetKind::MarkerDelete, opId, payload);
	}

	void NetSendMeasureAdd(const MeasureLine& line)
	{
		std::vector<BYTE> payload;
		payload.reserve(8);
		AppendU16(payload, line.A.X);
		AppendU16(payload, line.A.Y);
		AppendU16(payload, line.B.X);
		AppendU16(payload, line.B.Y);
		SendNetPacket(NetKind::MeasureAdd, line.Id, payload);
	}

	void NetSendMeasureDelete(uint32_t opId, uint32_t targetId)
	{
		std::vector<BYTE> payload;
		AppendU32(payload, targetId);
		SendNetPacket(NetKind::MeasureDelete, opId, payload);
	}

	void NetSendClear(uint32_t opId)
	{
		SendNetPacket(NetKind::Clear, opId, {});
	}

	void NetSendUndo(uint32_t targetId)
	{
		std::vector<BYTE> payload;
		AppendU32(payload, targetId);
		SendNetPacket(NetKind::Undo, NetNewOpId(), payload);
	}


	struct Hotkey
	{
		unsigned Mods = 0; // 1 = Ctrl, 2 = Shift, 4 = Alt
		unsigned Vk = 0;
	};

	struct HotkeySet
	{
		std::string Camera = "Ctrl+V";
		std::string Brush = "Ctrl+B";
		std::string Eraser = "Ctrl+E";
		std::string Ruler = "Ctrl+R";
		std::string Text = "Ctrl+T";
		std::string Marker = "Ctrl+P";
		std::string Color = "Ctrl+C";
		std::string Width = "Ctrl+W";
		std::string Clear = "Ctrl+Delete";
		std::string Undo = "Ctrl+Z";
		std::string Close = "Ctrl+M";
	};

	constexpr int HotkeyCount = 11;
	HotkeySet HotkeyText;
	Hotkey Hotkeys[HotkeyCount];
	bool PrevHotkeyDown[HotkeyCount] = {};
	int LastHotkeyFrame = -1;

	enum class Popup { None, Colors, Widths };
	Popup ActivePopup = Popup::None;
	int ToolbarLogCount = 0;

		struct ToolbarLayout
	{
		RectangleStruct Panel {};
		RectangleStruct Buttons[9] {};
	};

	ToolbarLayout ComputeToolbar(const RectangleStruct& view, const RectangleStruct& dst)
	{
		constexpr int Button = 44;
		constexpr int Gap = 6;
		constexpr int Pad = 6;
		constexpr int Count = 9;

		const int width = Button + Pad * 2;
		const int height = Count * Button + (Count - 1) * Gap + Pad * 2;

		int x = dst.X + dst.Width + 16;

		if (x + width > view.X + view.Width - 8)
			x = view.X + view.Width - width - 8;

		x = std::max(x, view.X + 8);

		int y = dst.Y + 8;

		if (y + height > view.Y + view.Height - 8)
			y = view.Y + view.Height - height - 8;

		y = std::max(y, view.Y + 8);

		ToolbarLayout layout {};
		layout.Panel = RectangleStruct { x, y, width, height };

		for (int i = 0; i < Count; ++i)
			layout.Buttons[i] = RectangleStruct { x + Pad, y + Pad + i * (Button + Gap), Button, Button };

		return layout;
	}

	struct PopupLayout
	{
		RectangleStruct Panel {};
		RectangleStruct Items[8] {};
		int Count = 0;
	};

	PopupLayout ComputePopup(Popup popup, const ToolbarLayout& bar, const RectangleStruct& view)
	{
		PopupLayout out {};

		if (popup == Popup::None)
			return out;

		const int anchor = popup == Popup::Colors ? 6 : 7;
		const int item = popup == Popup::Colors ? 36 : 40;
		const int gap = 6;
		const int pad = 6;
		const int cols = popup == Popup::Colors ? 4 : 1;
		const int rows = popup == Popup::Colors ? 2 : 7;
		const int count = cols * rows;

		const int width = cols * item + (cols - 1) * gap + pad * 2;
		const int height = rows * item + (rows - 1) * gap + pad * 2;

		int x = bar.Buttons[anchor].X - width - 8;

		if (x < view.X + 8)
			x = bar.Buttons[anchor].X + bar.Buttons[anchor].Width + 8;

		int y = bar.Buttons[anchor].Y;

		if (y + height > view.Y + view.Height - 8)
			y = view.Y + view.Height - height - 8;

		y = std::max(y, view.Y + 8);

		out.Panel = RectangleStruct { x, y, width, height };
		out.Count = count;

		for (int i = 0; i < count; ++i)
		{
			const int c = i % cols;
			const int r = i / cols;
			out.Items[i] = RectangleStruct { x + pad + c * (item + gap), y + pad + r * (item + gap), item, item };
		}

		return out;
	}

	inline void Line(DSurface* s, int x0, int y0, int x1, int y1, int color)
	{
		Point2D a { x0, y0 };
		Point2D b { x1, y1 };
		s->DrawLine(&a, &b, color);
	}

	// FillRectEx does not paint on the composited surface the overlay draws into, while
	// FillRectTrans does (the dim backdrop and the tool panel use it successfully).
	inline void FillBox(DSurface* s, const RectangleStruct& rect, unsigned rgb, int opacity = 100)
	{
		RectangleStruct area = rect;
		ColorStruct colour {
			static_cast<BYTE>((rgb >> 16) & 0xFF),
			static_cast<BYTE>((rgb >> 8) & 0xFF),
			static_cast<BYTE>(rgb & 0xFF)
		};

		s->FillRectTrans(&area, &colour, opacity);
	}

	void DrawToolIcon(DSurface* s, int index, const RectangleStruct& r, int color)
	{
		const int cx = r.X + r.Width / 2;
		const int cy = r.Y + r.Height / 2;
		const int s1 = 14;
		const int s2 = 8;

		switch (static_cast<ToolButton>(index))
		{
		case ToolButton::Camera:
			Line(s, cx - s1, cy, cx + s1, cy, color);
			Line(s, cx, cy - s1, cx, cy + s1, color);
			Line(s, cx - 5, cy - 5, cx + 5, cy - 5, color);
			Line(s, cx + 5, cy - 5, cx + 5, cy + 5, color);
			Line(s, cx + 5, cy + 5, cx - 5, cy + 5, color);
			Line(s, cx - 5, cy + 5, cx - 5, cy - 5, color);
			break;

		case ToolButton::Brush:
			Line(s, cx - s2, cy + s2, cx + s2, cy - s2, color);
			Line(s, cx - s2, cy + s2 - 1, cx + s2 - 1, cy - s2, color);
			Line(s, cx - s2 + 1, cy + s2, cx + s2, cy - s2 + 1, color);
			Line(s, cx - s2 - 1, cy + s2 + 1, cx - s2 + 2, cy + s2 - 2, color);
			break;

		case ToolButton::Eraser:
			Line(s, cx - 9, cy + 7, cx - 1, cy - 8, color);
			Line(s, cx - 2, cy + 8, cx + 8, cy - 5, color);
			Line(s, cx - 9, cy + 7, cx - 2, cy + 8, color);
			Line(s, cx - 1, cy - 8, cx + 8, cy - 5, color);
			Line(s, cx - 5, cy + 9, cx + 9, cy + 9, color);
			break;

		case ToolButton::Ruler:
			Line(s, cx - 9, cy + 9, cx + 9, cy - 9, color);
			Line(s, cx - 5, cy + 6, cx - 2, cy + 9, color);
			Line(s, cx, cy + 1, cx + 3, cy + 4, color);
			Line(s, cx + 5, cy - 4, cx + 8, cy - 1, color);
			break;

		case ToolButton::Text:
			Line(s, cx - 9, cy + 9, cx, cy - 9, color);
			Line(s, cx, cy - 9, cx + 9, cy + 9, color);
			Line(s, cx - 5, cy + 2, cx + 5, cy + 2, color);
			break;

		case ToolButton::Marker:
			Line(s, cx - 9, cy - 6, cx + 9, cy - 6, color);
			Line(s, cx - 9, cy - 6, cx, cy + 9, color);
			Line(s, cx + 9, cy - 6, cx, cy + 9, color);
			break;

		case ToolButton::Color:
		{
			RectangleStruct clip = r;
			RectangleStruct inner { r.X + 8, r.Y + 8, r.Width - 16, r.Height - 16 };
			FillBox(s, inner, BrushRgb);
			s->DrawRectEx(&clip, &inner, color);
			break;
		}

		case ToolButton::Width:
		{
			const int h = std::max(2, BrushWidth);
			RectangleStruct clip = r;
			RectangleStruct bar { r.X + 8, cy - h / 2, r.Width - 16, h };
			FillBox(s, bar, 0xEBEBEB);
			break;
		}

		case ToolButton::Clear:
			Line(s, cx - 8, cy - 8, cx + 8, cy + 8, color);
			Line(s, cx + 8, cy - 8, cx - 8, cy + 8, color);
			Line(s, cx - 8, cy - 8 + 1, cx + 8, cy + 8 + 1, color);
			Line(s, cx + 8, cy - 8 + 1, cx - 8, cy + 8 + 1, color);
			break;

		default:
			break;
		}
	}

	void DrawToolbar(DSurface* s, const RectangleStruct& view, const RectangleStruct& dst)
	{
		ToolbarLayout layout = ComputeToolbar(view, dst);

		ColorStruct backdrop { 0, 0, 0 };
		s->FillRectTrans(&layout.Panel, &backdrop, 70);

		RectangleStruct clip = layout.Panel;
		s->DrawRectEx(&clip, &layout.Panel, Drawing::RGB_To_Int(190, 190, 190));

		for (int i = 0; i < 9; ++i)
		{
			RectangleStruct r = layout.Buttons[i];

			if (i < 6 && static_cast<int>(CurrentTool) == i)
			{
				ColorStruct highlight { 60, 120, 220 };
				s->FillRectTrans(&r, &highlight, 55);
				RectangleStruct sel = r;
				s->DrawRectEx(&r, &sel, Drawing::RGB_To_Int(140, 210, 255));
			}

			DrawToolIcon(s, i, r, Drawing::RGB_To_Int(235, 235, 235));
		}
	}

	void DrawPopup(DSurface* s, const ToolbarLayout& bar, const RectangleStruct& view)
	{
		if (ActivePopup == Popup::None)
			return;

		const PopupLayout layout = ComputePopup(ActivePopup, bar, view);

		if (layout.Count <= 0)
			return;

		RectangleStruct panel = layout.Panel;
		FillBox(s, panel, 0x000000, 85);

		RectangleStruct outer { panel.X - 1, panel.Y - 1, panel.Width + 2, panel.Height + 2 };
		s->DrawRectEx(&outer, &panel, Drawing::RGB_To_Int(235, 235, 235));
		s->DrawRectEx(&panel, &panel, Drawing::RGB_To_Int(90, 90, 90));

		for (int i = 0; i < layout.Count; ++i)
		{
			RectangleStruct item = layout.Items[i];

			if (ActivePopup == Popup::Colors)
			{
				const unsigned colour = Palette[i];

				FillBox(s, item, colour);
				s->DrawRectEx(&panel, &item, Drawing::RGB_To_Int(20, 20, 20));

				if (colour == BrushRgb)
				{
					RectangleStruct ring { item.X - 2, item.Y - 2, item.Width + 4, item.Height + 4 };
					s->DrawRectEx(&panel, &ring, Drawing::RGB_To_Int(255, 255, 0));
				}
			}
			else
			{
				const int h = std::max(2, Widths[i]);
				RectangleStruct barRect { item.X + 4, item.Y + item.Height / 2 - h / 2, item.Width - 8, h };
				FillBox(s, barRect, 0xEBEBEB);

				if (Widths[i] == BrushWidth)
				{
					RectangleStruct ring { item.X - 2, item.Y - 2, item.Width + 4, item.Height + 4 };
					s->DrawRectEx(&panel, &ring, Drawing::RGB_To_Int(255, 255, 0));
				}
			}
		}
	}

	Point2D ToNormalised(const Point2D& point, const RectangleStruct& dst)
	{
		const double u = static_cast<double>(point.X - dst.X) / dst.Width;
		const double v = static_cast<double>(point.Y - dst.Y) / dst.Height;

		return Point2D {
			static_cast<int>(std::lround(std::clamp(u, 0.0, 1.0) * 10000.0)),
			static_cast<int>(std::lround(std::clamp(v, 0.0, 1.0) * 10000.0))
		};
	}

	// Normalised coordinates are stored as 0..10000 integers to keep Point2D.
	inline double NormX(const Point2D& p) { return p.X / 10000.0; }
	inline double NormY(const Point2D& p) { return p.Y / 10000.0; }

	bool MeasureCells(const RectangleStruct& src, const Point2D& a, const Point2D& b, double& cells)
	{
		int ax = 0;
		int ay = 0;
		int bx = 0;
		int by = 0;

		const int radAX = static_cast<int>(std::lround(NormX(a) * src.Width));
		const int radAY = static_cast<int>(std::lround(NormY(a) * src.Height));
		const int radBX = static_cast<int>(std::lround(NormX(b) * src.Width));
		const int radBY = static_cast<int>(std::lround(NormY(b) * src.Height));

		if (!RadarToCell(src, radAX, radAY, ax, ay) || !RadarToCell(src, radBX, radBY, bx, by))
			return false;

		const double dx = bx - ax;
		const double dy = by - ay;
		cells = std::sqrt(dx * dx + dy * dy);
		return true;
	}

	void DrawMeasure(DSurface* s, const RectangleStruct& src, const RectangleStruct& dst,
		const Point2D& a, const Point2D& b, int color, GdiTextBitmap& label, int& labelTenths)
	{
		const int ax = dst.X + static_cast<int>(std::lround(NormX(a) * dst.Width));
		const int ay = dst.Y + static_cast<int>(std::lround(NormY(a) * dst.Height));
		const int bx = dst.X + static_cast<int>(std::lround(NormX(b) * dst.Width));
		const int by = dst.Y + static_cast<int>(std::lround(NormY(b) * dst.Height));

		Line(s, ax, ay, bx, by, color);
		Line(s, ax, ay + 1, bx, by + 1, color);

		RectangleStruct clip = dst;
		RectangleStruct capA { ax - 2, ay - 2, 5, 5 };
		RectangleStruct capB { bx - 2, by - 2, 5, 5 };
		s->DrawRectEx(&clip, &capA, color);
		s->DrawRectEx(&clip, &capB, color);

		double cells = 0.0;

		if (!MeasureCells(src, a, b, cells))
			return;

		const int tenths = static_cast<int>(std::lround(cells * 10.0));

		if (tenths != labelTenths || label.Pixels.empty())
		{
			// Unit suffix is localised through the string table; English is the
			// default so it reads correctly even when no CSF entry is present.
			const wchar_t* suffix = GeneralUtils::LoadStringUnlessMissing("TXT_TACTICAL_MAP_CELLS", L"cells");
			wchar_t buffer[64] {};
			swprintf_s(buffer, L"%.1f %s", cells, suffix);

			const unsigned rgb = static_cast<unsigned>(((color & 0xFF) << 16) | (color & 0xFF00) | ((color >> 16) & 0xFF));

			if (TacticalMapGdi::Render(buffer, 14, rgb, label))
				labelTenths = tenths;
		}

		if (label.Pixels.empty())
			return;

		// The label is centred on the midpoint of the line.
		const int tx = (ax + bx) / 2 - label.Width / 2;
		const int ty = (ay + by) / 2 - label.Height / 2;

		TacticalMapGdi::Blit(s, label, tx, ty, dst);
	}

	bool PointIn(const Point2D& p, const RectangleStruct& r)
	{
		return p.X >= r.X && p.Y >= r.Y && p.X < r.X + r.Width && p.Y < r.Y + r.Height;
	}

	void StartStroke(const Point2D& point, const RectangleStruct& dst)
	{
		const Point2D norm = ToNormalised(point, dst);
		const int mx = static_cast<int>(std::lround(NormX(norm) * TacticalMapCanvas::Size));
		const int my = static_cast<int>(std::lround(NormY(norm) * TacticalMapCanvas::Size));
		const int screenWidth = CurrentTool == Tool::Eraser
			? std::max(4, TacticalMap::Settings.EraserRadius)
			: BrushWidth;
		const int radius = std::max(1, static_cast<int>(std::lround(screenWidth * 0.5 * TacticalMapCanvas::Size / dst.Width)));

		// A remote stroke cannot share the canvas with the local one.
		if (RemoteStrokeActive)
		{
			TacticalMapCanvas::EndStroke();
			RemoteStrokeActive = false;
		}

		TacticalMapCanvas::BeginStroke(mx, my, radius, BrushRgb, CurrentTool == Tool::Eraser);
		StrokeActive = true;

		CurrentStrokeOp = NetNewOpId();
		LastStrokeSendFrame = Unsorted::CurrentFrame;
		LastStrokeSendX = mx;
		LastStrokeSendY = my;
		NetSendStrokeBegin(static_cast<int>(BrushRgb), radius, CurrentTool == Tool::Eraser, mx, my);
	}

	void ContinueStroke(const Point2D& point, const RectangleStruct& dst)
	{
		const Point2D norm = ToNormalised(point, dst);
		const int mx = static_cast<int>(std::lround(NormX(norm) * TacticalMapCanvas::Size));
		const int my = static_cast<int>(std::lround(NormY(norm) * TacticalMapCanvas::Size));

		TacticalMapCanvas::ContinueStroke(mx, my);

		// Real-time sync, throttled by distance and frame so a fast scribble does
		// not flood the event queue.
		const int dx = mx - LastStrokeSendX;
		const int dy = my - LastStrokeSendY;

		if (dx * dx + dy * dy >= 9 || Unsorted::CurrentFrame - LastStrokeSendFrame >= 2)
		{
			LastStrokeSendFrame = Unsorted::CurrentFrame;
			LastStrokeSendX = mx;
			LastStrokeSendY = my;
			NetSendStrokePoint(mx, my);
		}
	}

	LRESULT CALLBACK TacticalMapWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

	void EnsureWndProc(HWND hwnd)
	{
		if (WndProcInstalled || !hwnd)
			return;

		OldWndProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
			hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&TacticalMapWndProc)));

		GameWindow = hwnd;
		WndProcInstalled = OldWndProc != nullptr;
	}

	void SetImePosition(HWND hwnd, int x, int y)
	{
		HIMC himc = ImmGetContext(hwnd);

		if (!himc)
		{
			himc = ImmCreateContext();

			if (himc)
				ImmAssociateContext(hwnd, himc);
		}

		if (!himc)
			return;

		COMPOSITIONFORM form {};
		form.dwStyle = CFS_POINT;
		form.ptCurrentPos.x = x;
		form.ptCurrentPos.y = y;
		ImmSetCompositionWindow(himc, &form);
		ImmReleaseContext(hwnd, himc);
	}

	void AppendEditText(const std::wstring& text)
	{
		for (const wchar_t ch : text)
		{
			if (ch == L'\r' || ch == L'\n' || ch == L'\b')
				continue;

			if (EditValue.size() >= 40)
				break;

			EditValue.push_back(ch);
		}
	}

	void BeginEdit(const Point2D& point, const RectangleStruct& dst)
	{
		EditingText = true;
		EditPos = ToNormalised(point, dst);
		EditValue.clear();
		EditComposition.clear();
		ImeComposing = false;
		EditRgb = BrushRgb;
		EditSize = TextSizes[std::clamp(WidthIndex, 0, 6)];

		if (HWND hwnd = Game::hWnd)
		{
			EnsureWndProc(hwnd);
			SetImePosition(hwnd, point.X, point.Y);
		}
	}

	void CommitText()
	{
		if (!EditingText)
			return;

		EditingText = false;
		EditComposition.clear();
		ImeComposing = false;

		std::wstring value = EditValue;
		EditValue.clear();

		while (!value.empty() && (value.back() == L' ' || value.back() == L'\t'))
			value.pop_back();

		if (value.empty())
			return;

		TextItem item;
		item.Pos = EditPos;
		item.Value = std::move(value);
		item.Rgb = EditRgb;
		item.Size = EditSize;
		item.Id = NetNewOpId();
		TacticalMapGdi::Render(item.Value, item.Size, item.Rgb, item.Bitmap);
		Texts.push_back(std::move(item));
		NetSendTextAdd(Texts.back());

		UndoAction action;
		action.Kind = UndoKind::TextAdd;
		action.OpId = Texts.back().Id;
		action.TextIndex = static_cast<int>(Texts.size()) - 1;
		PushUndo(std::move(action));
	}

	void CancelText()
	{
		EditingText = false;
		EditValue.clear();
		EditComposition.clear();
		ImeComposing = false;
	}

	void PushUndo(UndoAction&& action)
	{
		action.Bytes = action.Pixels.size() * sizeof(std::pair<int, DWORD>)
			+ action.Text.Bitmap.Pixels.size() * sizeof(DWORD)
			+ action.CanvasSnapshot.size() * sizeof(DWORD)
			+ action.TextsSnapshot.size() * 256
			+ action.MarkersSnapshot.size() * sizeof(MarkerItem)
			+ action.MeasuresSnapshot.size() * (sizeof(MeasureLine) + 256)
			+ 64;

		UndoHistory.push_back(std::move(action));
		UndoBytes += UndoHistory.back().Bytes;

		// Depth limit plus a memory budget so "clear" snapshots cannot pile up.
		while (UndoHistory.size() > 50 || (UndoBytes > 64u * 1024u * 1024u && UndoHistory.size() > 1))
		{
			UndoBytes -= UndoHistory.front().Bytes;
			UndoHistory.erase(UndoHistory.begin());
		}
	}

	void ClearAll()
	{
		const bool hasContent = TacticalMapCanvas::HasContent() || !Texts.empty() || !Markers.empty() || !Measures.empty();

		if (!hasContent)
			return;

		const uint32_t opId = NetNewOpId();

		UndoAction action;
		action.Kind = UndoKind::Clear;
		action.OpId = opId;

		if (TacticalMapCanvas::HasContent())
			TacticalMapCanvas::Save(action.CanvasSnapshot);

		action.TextsSnapshot = Texts;
		action.MarkersSnapshot = Markers;
		action.MeasuresSnapshot = Measures;
		PushUndo(std::move(action));
		NetSendClear(opId);

		TacticalMapCanvas::Reset();
		Texts.clear();
		Markers.clear();
		Measures.clear();
	}

	void PerformUndo()
	{
		if (UndoHistory.empty())
			return;

		UndoAction action = std::move(UndoHistory.back());
		UndoHistory.pop_back();

		if (UndoBytes >= action.Bytes)
			UndoBytes -= action.Bytes;
		else
			UndoBytes = 0;

		switch (action.Kind)
		{
		case UndoKind::Canvas:
			TacticalMapCanvas::ApplyEdits(action.Pixels);
			break;

		case UndoKind::TextAdd:
			if (action.TextIndex >= 0 && action.TextIndex < static_cast<int>(Texts.size()))
				Texts.erase(Texts.begin() + action.TextIndex);
			break;

		case UndoKind::TextDelete:
		{
			const int index = std::clamp(action.TextIndex, 0, static_cast<int>(Texts.size()));
			Texts.insert(Texts.begin() + index, action.Text);
			break;
		}

		case UndoKind::MarkerAdd:
			if (action.MarkerIndex >= 0 && action.MarkerIndex < static_cast<int>(Markers.size()))
				Markers.erase(Markers.begin() + action.MarkerIndex);
			break;

		case UndoKind::MarkerDelete:
		{
			const int index = std::clamp(action.MarkerIndex, 0, static_cast<int>(Markers.size()));
			Markers.insert(Markers.begin() + index, action.Marker);
			break;
		}

		case UndoKind::MeasureAdd:
			if (action.MeasureIndex >= 0 && action.MeasureIndex < static_cast<int>(Measures.size()))
				Measures.erase(Measures.begin() + action.MeasureIndex);
			break;

		case UndoKind::MeasureDelete:
		{
			const int index = std::clamp(action.MeasureIndex, 0, static_cast<int>(Measures.size()));
			Measures.insert(Measures.begin() + index, action.Measure);
			break;
		}

		case UndoKind::Clear:
			TacticalMapCanvas::Restore(action.CanvasSnapshot);
			Texts = action.TextsSnapshot;
			Markers = action.MarkersSnapshot;
			Measures = action.MeasuresSnapshot;
			break;

		default:
			break;
		}

		if (action.OpId)
			NetSendUndo(action.OpId);
	}

	unsigned ParseKeyName(const std::string& name)
	{
		if (name.size() == 1)
		{
			const char c = static_cast<char>(toupper(static_cast<unsigned char>(name[0])));

			if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
				return static_cast<unsigned>(c);
		}

		if (name.size() >= 2 && (name[0] == 'F' || name[0] == 'f'))
		{
			const int n = atoi(name.c_str() + 1);

			if (n >= 1 && n <= 12)
				return static_cast<unsigned>(VK_F1 + n - 1);
		}

		if (_stricmp(name.c_str(), "Delete") == 0) return VK_DELETE;
		if (_stricmp(name.c_str(), "Insert") == 0) return VK_INSERT;
		if (_stricmp(name.c_str(), "Home") == 0) return VK_HOME;
		if (_stricmp(name.c_str(), "End") == 0) return VK_END;
		if (_stricmp(name.c_str(), "PageUp") == 0) return VK_PRIOR;
		if (_stricmp(name.c_str(), "PageDown") == 0) return VK_NEXT;
		if (_stricmp(name.c_str(), "Space") == 0) return VK_SPACE;
		if (_stricmp(name.c_str(), "Tab") == 0) return VK_TAB;
		if (_stricmp(name.c_str(), "Enter") == 0) return VK_RETURN;
		if (_stricmp(name.c_str(), "Backspace") == 0) return VK_BACK;
		if (_stricmp(name.c_str(), "Up") == 0) return VK_UP;
		if (_stricmp(name.c_str(), "Down") == 0) return VK_DOWN;
		if (_stricmp(name.c_str(), "Left") == 0) return VK_LEFT;
		if (_stricmp(name.c_str(), "Right") == 0) return VK_RIGHT;

		return 0;
	}

	Hotkey ParseHotkey(const std::string& text)
	{
		Hotkey hotkey;
		size_t start = 0;

		while (start <= text.size())
		{
			const size_t plus = text.find('+', start);
			std::string part = text.substr(start, plus == std::string::npos ? std::string::npos : plus - start);

			while (!part.empty() && isspace(static_cast<unsigned char>(part.front())))
				part.erase(part.begin());

			while (!part.empty() && isspace(static_cast<unsigned char>(part.back())))
				part.pop_back();

			if (_stricmp(part.c_str(), "Ctrl") == 0)
				hotkey.Mods |= 1;
			else if (_stricmp(part.c_str(), "Shift") == 0)
				hotkey.Mods |= 2;
			else if (_stricmp(part.c_str(), "Alt") == 0)
				hotkey.Mods |= 4;
			else
				hotkey.Vk = ParseKeyName(part);

			if (plus == std::string::npos)
				break;

			start = plus + 1;
		}

		return hotkey;
	}

	void ParseHotkeys()
	{
		Hotkeys[0] = ParseHotkey(HotkeyText.Camera);
		Hotkeys[1] = ParseHotkey(HotkeyText.Brush);
		Hotkeys[2] = ParseHotkey(HotkeyText.Eraser);
		Hotkeys[3] = ParseHotkey(HotkeyText.Ruler);
		Hotkeys[4] = ParseHotkey(HotkeyText.Text);
		Hotkeys[5] = ParseHotkey(HotkeyText.Marker);
		Hotkeys[6] = ParseHotkey(HotkeyText.Color);
		Hotkeys[7] = ParseHotkey(HotkeyText.Width);
		Hotkeys[8] = ParseHotkey(HotkeyText.Clear);
		Hotkeys[9] = ParseHotkey(HotkeyText.Undo);
		Hotkeys[10] = ParseHotkey(HotkeyText.Close);
	}

	bool HotkeyHeld(const Hotkey& hotkey)
	{
		if (!hotkey.Vk)
			return false;

		const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
		const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
		const bool alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;

		if (((hotkey.Mods & 1) != 0) != ctrl)
			return false;

		if (((hotkey.Mods & 2) != 0) != shift)
			return false;

		if (((hotkey.Mods & 4) != 0) != alt)
			return false;

		return (GetAsyncKeyState(static_cast<int>(hotkey.Vk)) & 0x8000) != 0;
	}

	// Called when the overlay opens: the key that opened it must not be mistaken for
	// a fresh press of a tactical hotkey (e.g. Ctrl+M opening and closing at once).
	void SyncHotkeyStates()
	{
		for (int i = 0; i < HotkeyCount; ++i)
			PrevHotkeyDown[i] = HotkeyHeld(Hotkeys[i]);
	}

	void ApplyHotkey(int index)
	{
		switch (index)
		{
		case 0: CurrentTool = Tool::Camera; break;
		case 1: CurrentTool = Tool::Brush; break;
		case 2: CurrentTool = Tool::Eraser; break;
		case 3: CurrentTool = Tool::Ruler; break;
		case 4: CurrentTool = Tool::Text; break;
		case 5: CurrentTool = Tool::Marker; break;

		case 6:
		{
			constexpr int ColorCount = sizeof(Palette) / sizeof(Palette[0]);
			ColorIndex = (ColorIndex + 1) % ColorCount;
			BrushRgb = Palette[ColorIndex];
			break;
		}

		case 7:
		{
			constexpr int WidthCount = sizeof(Widths) / sizeof(Widths[0]);
			WidthIndex = (WidthIndex + 1) % WidthCount;
			BrushWidth = Widths[WidthIndex];
			break;
		}

		case 8:
			ClearAll();
			break;

		case 9:
			PerformUndo();
			break;

		case 10:
			TacticalMap::Close();
			break;

		default:
			break;
		}

		if (index <= 5)
		{
			if (StrokeActive)
				NetSendStrokeEnd();

			StrokeActive = false;
			CurrentStrokeOp = 0;
			MeasureActive = false;
			TacticalMapCanvas::EndStroke();
			ActivePopup = Popup::None;
		}
	}

	void PollHotkeys()
	{
		if (LastHotkeyFrame == Unsorted::CurrentFrame)
			return;

		LastHotkeyFrame = Unsorted::CurrentFrame;

		if (EditingText)
		{
			for (int i = 0; i < HotkeyCount; ++i)
				PrevHotkeyDown[i] = false;

			return;
		}

		for (int i = 0; i < HotkeyCount; ++i)
		{
			const bool down = HotkeyHeld(Hotkeys[i]);

			if (down && !PrevHotkeyDown[i])
				ApplyHotkey(i);

			PrevHotkeyDown[i] = down;
		}
	}

	void DrawMarker(DSurface* s, const MarkerItem& marker, const RectangleStruct& dst)
	{
		const int tipX = dst.X + static_cast<int>(std::lround(NormX(marker.Pos) * dst.Width));
		const int tipY = dst.Y + static_cast<int>(std::lround(NormY(marker.Pos) * dst.Height));

		// Each marker keeps the colour and size it was placed with; changing the
		// brush later only affects newly placed markers.
		const int height = std::max(8, marker.Size);
		const int width = height + 2;

		const int fill = Drawing::RGB_To_Int((marker.Rgb >> 16) & 0xFF, (marker.Rgb >> 8) & 0xFF, marker.Rgb & 0xFF);
		const int black = Drawing::RGB_To_Int(0, 0, 0);

		for (int row = 0; row < height; ++row)
		{
			const int half = width / 2 * (height - row) / height;
			const int y = tipY - height + row;
			Line(s, tipX - half, y, tipX + half, y, fill);
		}

		Line(s, tipX - width / 2, tipY - height, tipX, tipY, black);
		Line(s, tipX + width / 2, tipY - height, tipX, tipY, black);
		Line(s, tipX - width / 2, tipY - height, tipX + width / 2, tipY - height, black);
	}

	void DrawTextItem(DSurface* s, TextItem& item, const RectangleStruct& dst)
	{
		if (item.Bitmap.Pixels.empty())
			TacticalMapGdi::Render(item.Value, item.Size, item.Rgb, item.Bitmap);

		const int x = dst.X + static_cast<int>(std::lround(NormX(item.Pos) * dst.Width));
		const int y = dst.Y + static_cast<int>(std::lround(NormY(item.Pos) * dst.Height));

		TacticalMapGdi::Blit(s, item.Bitmap, x, y, dst);
		item.Bounds = RectangleStruct { x, y, item.Bitmap.Width, item.Bitmap.Height };
	}

	void DrawEditor(DSurface* s, const RectangleStruct& dst)
	{
		if (!EditingText)
			return;

		const int x = dst.X + static_cast<int>(std::lround(NormX(EditPos) * dst.Width));
		const int y = dst.Y + static_cast<int>(std::lround(NormY(EditPos) * dst.Height));

		std::wstring preview = EditValue;

		if (!EditComposition.empty())
			preview += EditComposition;

		static GdiTextBitmap cached;
		static std::wstring cachedText;
		static int cachedSize = 0;
		static unsigned cachedRgb = 0;

		if (!preview.empty() && (preview != cachedText || cachedSize != EditSize || cachedRgb != EditRgb))
		{
			if (TacticalMapGdi::Render(preview, EditSize, EditRgb, cached))
			{
				cachedText = preview;
				cachedSize = EditSize;
				cachedRgb = EditRgb;
			}
		}

		int caretX = x;

		if (!preview.empty() && !cached.Pixels.empty())
		{
			TacticalMapGdi::Blit(s, cached, x, y, dst);
			caretX = x + cached.Width - 2;
		}

		if ((Unsorted::CurrentFrame & 0x10) != 0)
			Line(s, caretX, y, caretX, y + EditSize, Drawing::RGB_To_Int(255, 255, 255));
	}

	void ResetInputState()
	{
		LastHandledFrame = -1;
		ClickLogCount = 0;
		MoveLogCount = 0;
		RadarLogCount = 0;
		PinValid = false;
		HasPendingClick = false;
		PrevLeftDown = false;
		PrevRightDown = false;

		if (StrokeActive)
			NetSendStrokeEnd();

		StrokeActive = false;
		CurrentStrokeOp = 0;
		MeasureActive = false;
		RemoteStrokeActive = false;
		TacticalMapCanvas::EndStroke();
		ActivePopup = Popup::None;
		ToolbarLogCount = 0;
	}

	// Screen Y of the sidebar surface's top edge. SidebarBounds describes the cameo
	// area below the minimap, while the surface itself spans the window height.
	int SidebarOriginY()
	{
		const RectangleStruct& bounds = DSurface::SidebarBounds;

		if (DSurface::Sidebar)
			return bounds.Y + bounds.Height - DSurface::Sidebar->GetHeight();

		return bounds.Y;
	}

	bool PointInRect(const Point2D& point, const RectangleStruct& rect)
	{
		return point.X >= rect.X && point.Y >= rect.Y
			&& point.X < rect.X + rect.Width && point.Y < rect.Y + rect.Height;
	}

	// The minimap is rendered into the interface at RadarClass::unknown_rect_149C
	// (screen coordinates - confirmed against the engine's own radar math). Even if
	// the radar is offline the rect still holds the "no radar" graphic, so copying
	// it keeps the overlay consistent with the sidebar minimap by construction.
	bool ComputeRects(RectangleStruct& srcOut, RectangleStruct& dstOut)
	{
		if (!DSurface::Composite || !TacticalClass::Instance)
			return false;

		const RectangleStruct& radar = RadarClass::Instance.unknown_rect_149C;

		if (radar.Width < MinSourceSide || radar.Height < MinSourceSide)
			return false;

		const RectangleStruct composite = DSurface::Composite->GetRect();

		RectangleStruct src { radar.X, radar.Y, radar.Width, radar.Height };

		if (src.X < 0)
		{
			src.Width += src.X;
			src.X = 0;
		}

		if (src.Y < 0)
		{
			src.Height += src.Y;
			src.Y = 0;
		}

		src.Width = std::min(src.Width, composite.Width - src.X);
		src.Height = std::min(src.Height, composite.Height - src.Y);

		if (src.Width < MinSourceSide || src.Height < MinSourceSide)
			return false;

		RectangleStruct view = DSurface::ViewBounds;

		if (view.Width < MinDestSide || view.Height < MinDestSide)
			view = composite;

		const int fitW = std::max(MinDestSide, view.Width * std::clamp(TacticalMap::Settings.FitPercent, 25, 100) / 100);
		const int fitH = std::max(MinDestSide, view.Height * std::clamp(TacticalMap::Settings.FitPercent, 25, 100) / 100);

		int dstW = 0;
		int dstH = 0;

		if (TacticalMap::Settings.Scale > 0)
		{
			dstW = src.Width * TacticalMap::Settings.Scale;
			dstH = src.Height * TacticalMap::Settings.Scale;
		}
		else
		{
			const double factor = std::min(
				static_cast<double>(fitW) / src.Width,
				static_cast<double>(fitH) / src.Height);

			dstW = std::max(MinDestSide, static_cast<int>(src.Width * factor + 0.5));
			dstH = std::max(MinDestSide, static_cast<int>(src.Height * factor + 0.5));
		}

		// A fixed scale may not fit the viewport; keep the aspect ratio and shrink.
		if (dstW > view.Width)
		{
			dstH = std::max(MinDestSide, dstH * view.Width / dstW);
			dstW = view.Width;
		}

		if (dstH > view.Height)
		{
			dstW = std::max(MinDestSide, dstW * view.Height / dstH);
			dstH = view.Height;
		}

		dstOut = RectangleStruct {
			view.X + (view.Width - dstW) / 2,
			view.Y + (view.Height - dstH) / 2,
			dstW,
			dstH
		};

		// Never write outside the surface, whatever the layout reports.
		dstOut.X = std::clamp(dstOut.X, 0, std::max(0, composite.Width - dstOut.Width));
		dstOut.Y = std::clamp(dstOut.Y, 0, std::max(0, composite.Height - dstOut.Height));

		srcOut = src;
		return true;
	}

	WORD Average16(WORD a, WORD b, WORD c, WORD d)
	{
		const int r = ((a >> 11) & 0x1F) + ((b >> 11) & 0x1F) + ((c >> 11) & 0x1F) + ((d >> 11) & 0x1F);
		const int g = ((a >> 5) & 0x3F) + ((b >> 5) & 0x3F) + ((c >> 5) & 0x3F) + ((d >> 5) & 0x3F);
		const int bl = (a & 0x1F) + (b & 0x1F) + (c & 0x1F) + (d & 0x1F);

		return static_cast<WORD>(((r >> 2) << 11) | ((g >> 2) << 5) | (bl >> 2));
	}

	DWORD Average32(DWORD a, DWORD b, DWORD c, DWORD d)
	{
		DWORD out = 0;

		for (int shift = 0; shift < 32; shift += 8)
		{
			const DWORD sum = ((a >> shift) & 0xFF) + ((b >> shift) & 0xFF)
				+ ((c >> shift) & 0xFF) + ((d >> shift) & 0xFF);

			out |= ((sum >> 2) & 0xFF) << shift;
		}

		return out;
	}

	bool ProjectCell(int x, int y, Point2D& out)
	{
		CellStruct cell { static_cast<short>(x), static_cast<short>(y) };
		CoordStruct crd = CellClass::Cell2Coord(cell, 0);

		// bRestrictToBound = false, otherwise the projection is clamped to the rect.
		return RadarClass::Instance.GetCrdOnRadar(&out, &crd, false) != nullptr;
	}

	// The minimap uses the engine's own isometric projection. GetCrdOnRadar does not
	// return rect-space pixels but coordinates in its own radar-image space, which is
	// scaled and offset relative to the minimap rect we copied. Normalise the click
	// pixel into that space with the bounding box of the projected map corners, then
	// invert the affine transform defined by three corners.
	bool RadarToCell(const RectangleStruct& src, int radarX, int radarY, int& cellX, int& cellY)
	{
		const RectangleStruct& map = MapClass::Instance.MapRect;

		if (map.Width < 2 || map.Height < 2 || src.Width < 2 || src.Height < 2)
			return false;

		Point2D p00 {};
		Point2D p10 {};
		Point2D p01 {};
		Point2D p11 {};

		if (!ProjectCell(map.X, map.Y, p00)
			|| !ProjectCell(map.X + map.Width - 1, map.Y, p10)
			|| !ProjectCell(map.X, map.Y + map.Height - 1, p01)
			|| !ProjectCell(map.X + map.Width - 1, map.Y + map.Height - 1, p11))
		{
			return false;
		}

		const double minX = std::min({ p00.X, p10.X, p01.X, p11.X });
		const double maxX = std::max({ p00.X, p10.X, p01.X, p11.X });
		const double minY = std::min({ p00.Y, p10.Y, p01.Y, p11.Y });
		const double maxY = std::max({ p00.Y, p10.Y, p01.Y, p11.Y });

		if (maxX - minX < 1.0 || maxY - minY < 1.0)
			return false;

		const double px = minX + static_cast<double>(radarX) * (maxX - minX) / src.Width;
		const double py = minY + static_cast<double>(radarY) * (maxY - minY) / src.Height;

		const double ux = static_cast<double>(p10.X - p00.X);
		const double uy = static_cast<double>(p10.Y - p00.Y);
		const double vx = static_cast<double>(p01.X - p00.X);
		const double vy = static_cast<double>(p01.Y - p00.Y);
		const double det = ux * vy - uy * vx;

		if (std::fabs(det) < 1e-6)
			return false;

		const double dx = px - static_cast<double>(p00.X);
		const double dy = py - static_cast<double>(p00.Y);

		const double s = (dx * vy - dy * vx) / det;
		const double t = (ux * dy - uy * dx) / det;

		cellX = map.X + static_cast<int>(std::lround(s * (map.Width - 1)));
		cellY = map.Y + static_cast<int>(std::lround(t * (map.Height - 1)));

		return true;
	}

	void LogProjection()
	{
		const RectangleStruct& map = MapClass::Instance.MapRect;

		if (map.Width < 2 || map.Height < 2)
			return;

		Point2D p00 {};
		Point2D p10 {};
		Point2D p01 {};
		Point2D p11 {};

		if (!ProjectCell(map.X, map.Y, p00)
			|| !ProjectCell(map.X + map.Width - 1, map.Y, p10)
			|| !ProjectCell(map.X, map.Y + map.Height - 1, p01)
			|| !ProjectCell(map.X + map.Width - 1, map.Y + map.Height - 1, p11))
		{
			return;
		}

		const int minX = std::min({ p00.X, p10.X, p01.X, p11.X });
		const int maxX = std::max({ p00.X, p10.X, p01.X, p11.X });
		const int minY = std::min({ p00.Y, p10.Y, p01.Y, p11.Y });
		const int maxY = std::max({ p00.Y, p10.Y, p01.Y, p11.Y });

		Debug::Log("[Phobos::TacticalMap] projection: p00={%d,%d} p10={%d,%d} p01={%d,%d} p11={%d,%d} bbox={%d,%d,%d,%d}\n",
			p00.X, p00.Y, p10.X, p10.Y, p01.X, p01.Y, p11.X, p11.Y,
			minX, minY, maxX, maxY);
	}

	// Software stretch blit. The source area is snapshotted before the destination
	// is written, so source and destination may even be the same surface.
	bool BlitScaled(DSurface* pSrc, const RectangleStruct& src, DSurface* pDst, const RectangleStruct& dst)
	{
		if (!pSrc || !pDst)
			return false;

		const int bpp = pDst->GetBytesPerPixel();

		if ((bpp != 2 && bpp != 4) || pSrc->GetBytesPerPixel() != bpp)
			return false;

		const size_t srcStride = static_cast<size_t>(src.Width) * bpp;

		thread_local std::vector<BYTE> snapshot;
		snapshot.resize(srcStride * src.Height);

		BYTE* const srcBase = static_cast<BYTE*>(pSrc->Lock(0, 0));

		if (!srcBase)
			return false;

		const int srcPitch = pSrc->GetPitch();

		for (int y = 0; y < src.Height; ++y)
		{
			std::memcpy(
				snapshot.data() + static_cast<size_t>(y) * srcStride,
				srcBase + static_cast<size_t>(src.Y + y) * srcPitch + static_cast<size_t>(src.X) * bpp,
				srcStride);
		}

		pSrc->Unlock();

		BYTE* const dstBase = static_cast<BYTE*>(pDst->Lock(0, 0));

		if (!dstBase)
			return false;

		const int dstPitch = pDst->GetPitch();

		thread_local std::vector<int> xMap;
		xMap.resize(dst.Width);

		for (int dx = 0; dx < dst.Width; ++dx)
			xMap[dx] = static_cast<int>(static_cast<long long>(dx) * src.Width / dst.Width);

		const bool smooth = TacticalMap::Settings.Smooth;

		for (int dy = 0; dy < dst.Height; ++dy)
		{
			const int sy = static_cast<int>(static_cast<long long>(dy) * src.Height / dst.Height);
			BYTE* const dstRow = dstBase + static_cast<size_t>(dst.Y + dy) * dstPitch + static_cast<size_t>(dst.X) * bpp;

			if (!smooth)
			{
				for (int dx = 0; dx < dst.Width; ++dx)
				{
					const BYTE* const pSourcePixel = snapshot.data() + static_cast<size_t>(sy) * srcStride + static_cast<size_t>(xMap[dx]) * bpp;

					if (bpp == 2)
						*reinterpret_cast<WORD*>(dstRow + static_cast<size_t>(dx) * 2) = *reinterpret_cast<const WORD*>(pSourcePixel);
					else
						*reinterpret_cast<DWORD*>(dstRow + static_cast<size_t>(dx) * 4) = *reinterpret_cast<const DWORD*>(pSourcePixel);
				}
			}
			else
			{
				const int sy2 = std::min(sy + 1, src.Height - 1);
				const BYTE* const row0 = snapshot.data() + static_cast<size_t>(sy) * srcStride;
				const BYTE* const row1 = snapshot.data() + static_cast<size_t>(sy2) * srcStride;

				for (int dx = 0; dx < dst.Width; ++dx)
				{
					const int sx = xMap[dx];
					const int sx2 = std::min(sx + 1, src.Width - 1);

					if (bpp == 2)
					{
						const WORD c = Average16(
							*reinterpret_cast<const WORD*>(row0 + static_cast<size_t>(sx) * 2),
							*reinterpret_cast<const WORD*>(row0 + static_cast<size_t>(sx2) * 2),
							*reinterpret_cast<const WORD*>(row1 + static_cast<size_t>(sx) * 2),
							*reinterpret_cast<const WORD*>(row1 + static_cast<size_t>(sx2) * 2));

						*reinterpret_cast<WORD*>(dstRow + static_cast<size_t>(dx) * 2) = c;
					}
					else
					{
						const DWORD c = Average32(
							*reinterpret_cast<const DWORD*>(row0 + static_cast<size_t>(sx) * 4),
							*reinterpret_cast<const DWORD*>(row0 + static_cast<size_t>(sx2) * 4),
							*reinterpret_cast<const DWORD*>(row1 + static_cast<size_t>(sx) * 4),
							*reinterpret_cast<const DWORD*>(row1 + static_cast<size_t>(sx2) * 4));

						*reinterpret_cast<DWORD*>(dstRow + static_cast<size_t>(dx) * 4) = c;
					}
				}
			}
		}

		pDst->Unlock();
		return true;
	}

	// Forward a click to the engine's own radar click handler so the overlay behaves
	// exactly like the sidebar minimap. It is called on the global radar gadget
	// (0xB04A10) with the RightPress flag, which only re-centres the camera and never
	// issues an order; the handler reads the mouse position from InputManager.
	void ApplyRadarClick(const Point2D& screen)
	{
		if (!TacticalClass::Instance)
			return;

		const Point2D before = TacticalClass::Instance->TacticalCoord1;
		bool moved = false;

		if (auto* const pInput = reinterpret_cast<int*>(InputManagerClass::Instance))
		{
			const int savedX = pInput[0];
			const int savedY = pInput[1];

			pInput[0] = screen.X;
			pInput[1] = screen.Y;

			using RadarActionFn = bool(__thiscall*)(void*, int, DWORD*, int);
			DWORD key = VK_RBUTTON;
			reinterpret_cast<RadarActionFn>(0x6539D0)(
				reinterpret_cast<void*>(static_cast<uintptr_t>(0xB04A10)), 0x10, &key, 0);

			pInput[0] = savedX;
			pInput[1] = savedY;

			const Point2D after = TacticalClass::Instance->TacticalCoord1;
			moved = after.X != before.X || after.Y != before.Y;
		}

		if (RadarLogCount < 5)
		{
			++RadarLogCount;
			const Point2D now = TacticalClass::Instance->TacticalCoord1;
			Debug::Log("[Phobos::TacticalMap] apply: screen={%d,%d} moved=%d camera={%d,%d}\n",
				screen.X, screen.Y, moved ? 1 : 0, now.X, now.Y);
		}

		if (moved)
			return;

		// Fallback: move the camera to the mapped cell directly.
		RectangleStruct src {};
		RectangleStruct dst {};

		if (!ComputeRects(src, dst))
			return;

		const int localX = screen.X - (DSurface::SidebarBounds.X + src.X);
		const int localY = screen.Y - (SidebarOriginY() + src.Y);

		int cellX = 0;
		int cellY = 0;

		if (!RadarToCell(src, localX, localY, cellX, cellY))
			return;

		const RectangleStruct& map = MapClass::Instance.MapRect;

		const CellStruct cell {
			static_cast<short>(std::clamp(cellX, map.X, map.X + map.Width - 1)),
			static_cast<short>(std::clamp(cellY, map.Y, map.Y + map.Height - 1))
		};

		CoordStruct coords = CellClass::Cell2Coord(cell, 0);
		TacticalClass::Instance->SetTacticalPosition(&coords);
		MapClass::Instance.MarkNeedsRedraw(2);
	}

	LRESULT CALLBACK TacticalMapWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
	{
		if (TacticalMap::HandleWindowMessage(hwnd, message, wParam, lParam))
			return 0;

		return CallWindowProcW(OldWndProc, hwnd, message, wParam, lParam);
	}
}

void TacticalMap::LoadFromINI(CCINIClass* pINI)
{
	// The game rewrites RA2MD.INI when options are saved and drops keys it does not
	// know, so the tactical map keeps its settings in a dedicated file next to the
	// executable (the CWD is the game directory).
	(void)pINI;

	constexpr const char* ini = "TacticalMap.ini";
	constexpr const char* section = "Settings";

	Settings.Enabled = GetPrivateProfileIntA(section, "Enabled", 1, ini) != 0;
	Settings.Scale = std::clamp(static_cast<int>(GetPrivateProfileIntA(section, "Scale", 0, ini)), 0, 8);
	Settings.FitPercent = std::clamp(static_cast<int>(GetPrivateProfileIntA(section, "FitPercent", 90, ini)), 25, 100);
	Settings.BackdropOpacity = std::clamp(static_cast<int>(GetPrivateProfileIntA(section, "BackdropOpacity", 60, ini)), 0, 100);
	Settings.Toolbar = GetPrivateProfileIntA(section, "Toolbar", 1, ini) != 0;
	Settings.BrushWidth = std::clamp(static_cast<int>(GetPrivateProfileIntA(section, "Brush.Width", 2, ini)), 1, 16);
	Settings.EraserRadius = std::clamp(static_cast<int>(GetPrivateProfileIntA(section, "Eraser.Radius", 12, ini)), 2, 64);
	Settings.ShareWithTeam = GetPrivateProfileIntA("Multiplayer", "ShareWithTeam", 1, ini) != 0;

	char buffer[64] {};

	auto readString = [&](const char* key, const char* fallback)
	{
		GetPrivateProfileStringA(section, key, fallback, buffer, sizeof(buffer), ini);
	};

	int r = Settings.BorderR;
	int g = Settings.BorderG;
	int b = Settings.BorderB;

	readString("Filter", "nearest");
	Settings.Smooth = _stricmp(buffer, "linear") == 0 || _stricmp(buffer, "smooth") == 0;

	readString("BorderColor", "220,220,220");

	if (std::sscanf(buffer, "%d , %d , %d", &r, &g, &b) == 3)
	{
		Settings.BorderR = std::clamp(r, 0, 255);
		Settings.BorderG = std::clamp(g, 0, 255);
		Settings.BorderB = std::clamp(b, 0, 255);
	}

	readString("Brush.Color", "255,240,80");

	if (std::sscanf(buffer, "%d , %d , %d", &r, &g, &b) == 3)
	{
		Settings.BrushR = std::clamp(r, 0, 255);
		Settings.BrushG = std::clamp(g, 0, 255);
		Settings.BrushB = std::clamp(b, 0, 255);
	}

	readString("Ruler.Color", "120,220,255");

	if (std::sscanf(buffer, "%d , %d , %d", &r, &g, &b) == 3)
	{
		Settings.RulerR = std::clamp(r, 0, 255);
		Settings.RulerG = std::clamp(g, 0, 255);
		Settings.RulerB = std::clamp(b, 0, 255);
	}

	BrushWidth = Settings.BrushWidth;
	BrushRgb = (static_cast<unsigned>(Settings.BrushR) << 16)
		| (static_cast<unsigned>(Settings.BrushG) << 8)
		| static_cast<unsigned>(Settings.BrushB);

	WidthIndex = 0;

	for (int i = 0; i < static_cast<int>(sizeof(Widths) / sizeof(Widths[0])); ++i)
	{
		if (Widths[i] == BrushWidth)
			WidthIndex = i;
	}

	char hotkeyBuffer[64] {};

	// Hotkeys live in their own file: the game rewrites RA2MD.INI on exit and drops
	// sections/keys it does not know about.
	auto readHotkey = [&](const char* key, std::string& value)
	{
		hotkeyBuffer[0] = '\0';
		GetPrivateProfileStringA("Hotkeys", key, value.c_str(), hotkeyBuffer, sizeof(hotkeyBuffer), "TacticalMap.ini");

		if (hotkeyBuffer[0])
			value = hotkeyBuffer;
	};

	readHotkey("Camera", HotkeyText.Camera);
	readHotkey("Brush", HotkeyText.Brush);
	readHotkey("Eraser", HotkeyText.Eraser);
	readHotkey("Ruler", HotkeyText.Ruler);
	readHotkey("Text", HotkeyText.Text);
	readHotkey("Marker", HotkeyText.Marker);
	readHotkey("Color", HotkeyText.Color);
	readHotkey("Width", HotkeyText.Width);
	readHotkey("Clear", HotkeyText.Clear);
	readHotkey("Undo", HotkeyText.Undo);
	readHotkey("Close", HotkeyText.Close);
	ParseHotkeys();

	if (!Settings.Enabled)
		Opened = false;
}

void TacticalMap::Toggle()
{
	if (!Settings.Enabled)
		return;

	Opened = !Opened;

	ResetInputState();

	if (Opened)
	{
		if (HWND hwnd = Game::hWnd)
			EnsureWndProc(hwnd);

		SyncHotkeyStates();

		if (TacticalClass::Instance)
		{
			PinnedCamera = TacticalClass::Instance->TacticalCoord1;
			PinValid = true;
		}

		const RectangleStruct& radar = RadarClass::Instance.unknown_rect_149C;
		const RectangleStruct view = DSurface::ViewBounds;
		const RectangleStruct composite = DSurface::Composite ? DSurface::Composite->GetRect() : RectangleStruct {};
		const int bpp = DSurface::Composite ? DSurface::Composite->GetBytesPerPixel() : 0;
		const RectangleStruct sidebar = DSurface::Sidebar ? DSurface::Sidebar->GetRect() : RectangleStruct {};
		const RectangleStruct map = MapClass::Instance.MapRect;

		Debug::Log("[Phobos::TacticalMap] open: radar={%d,%d,%d,%d} view={%d,%d,%d,%d} map={%d,%d,%d,%d} composite=%dx%d sidebar=%dx%d bpp=%d\n",
			radar.X, radar.Y, radar.Width, radar.Height,
			view.X, view.Y, view.Width, view.Height,
			map.X, map.Y, map.Width, map.Height,
			composite.Width, composite.Height,
			sidebar.Width, sidebar.Height, bpp);

		LogProjection();
	}
}

void TacticalMap::Close()
{
	Opened = false;
	CancelText();
	ResetInputState();
}

void TacticalMap::Draw()
{
	if (!Opened)
		return;

	// Cinema mode hides the sidebar, so there is no minimap to copy.
	if (Phobos::UI::CinemaMode || !HouseClass::CurrentPlayer)
	{
		Opened = false;
		return;
	}

	RectangleStruct src {};
	RectangleStruct dst {};

	if (!ComputeRects(src, dst))
		return;

	DSurface* const pSurface = DSurface::Composite;

	if (!pSurface)
		return;

	// Darken the tactical viewport only, the HUD stays readable.
	RectangleStruct view = DSurface::ViewBounds;

	if (Settings.BackdropOpacity > 0 && view.Width > 0 && view.Height > 0)
	{
		ColorStruct black { 0, 0, 0 };
		pSurface->FillRectTrans(&view, &black, std::clamp(Settings.BackdropOpacity, 0, 100));
	}

	// The minimap lives on DSurface::Sidebar: SidebarClass::Draw swaps
	// DSurface::Temp to it, so the radar (and the radar rect) is drawn there.
	// Fall back to the composite when the sidebar surface cannot hold the rect.
	DSurface* pSource = DSurface::Sidebar;

	if (!pSource || pSource->GetWidth() < src.X + src.Width || pSource->GetHeight() < src.Y + src.Height)
		pSource = pSurface;

	if (!BlitScaled(pSource, src, pSurface, dst))
		return;

	// The radar signal markers are drawn on the sidebar, so the enlarged copy
	// above already contains them at the correct proportion (and the camera view
	// box, which the engine draws after them, stays on top).

	// Freehand annotations, then measurements, then the frame and the tool panel.
	TacticalMapCanvas::Render(pSurface, dst);

	const int rulerColor = Drawing::RGB_To_Int(Settings.RulerR, Settings.RulerG, Settings.RulerB);

	for (auto& measure : Measures)
		DrawMeasure(pSurface, src, dst, measure.A, measure.B, rulerColor, measure.Label, measure.LabelTenths);

	if (MeasureActive)
	{
		static GdiTextBitmap previewLabel;
		static int previewTenths = -1;
		DrawMeasure(pSurface, src, dst, MeasureStart, MeasureEnd, rulerColor, previewLabel, previewTenths);
	}

	for (const auto& marker : Markers)
		DrawMarker(pSurface, marker, dst);

	for (auto& text : Texts)
		DrawTextItem(pSurface, text, dst);

	DrawEditor(pSurface, dst);

	const int borderColor = Drawing::RGB_To_Int(Settings.BorderR, Settings.BorderG, Settings.BorderB);
	RectangleStruct composite = pSurface->GetRect();
	pSurface->DrawRectEx(&composite, &dst, borderColor);

	if (Settings.Toolbar)
	{
		DrawToolbar(pSurface, DSurface::ViewBounds, dst);
		DrawPopup(pSurface, ComputeToolbar(DSurface::ViewBounds, dst), DSurface::ViewBounds);
	}
}

bool TacticalMap::HandleClick(const Point2D& point)
{
	RectangleStruct src {};
	RectangleStruct dst {};

	const bool haveRects = ComputeRects(src, dst);
	const bool inside = haveRects && PointInRect(point, dst);

	// Diagnostic: the first few clicks tell us whether the input path works and
	// whether the click landed on the map rectangle.
	if (ClickLogCount < 3)
	{
		++ClickLogCount;
		Debug::Log("[Phobos::TacticalMap] click: screen={%d,%d} dst={%d,%d,%d,%d} inside=%d\n",
			point.X, point.Y, dst.X, dst.Y, dst.Width, dst.Height, inside ? 1 : 0);
	}

	if (!inside)
		return false;

	const RectangleStruct& map = MapClass::Instance.MapRect;

	if (map.Width <= 0 || map.Height <= 0)
		return false;

	// Map the click back to a pixel of the source minimap rect, then to the screen
	// position of the real minimap. The sidebar surface spans the window height, so
	// only its X origin comes from SidebarBounds; its Y origin is computed from the
	// surface height (SidebarBounds itself describes the cameo area below the map).
	const int radarX = (point.X - dst.X) * src.Width / dst.Width;
	const int radarY = (point.Y - dst.Y) * src.Height / dst.Height;

	const int screenX = DSurface::SidebarBounds.X + src.X + radarX;
	const int screenY = SidebarOriginY() + src.Y + radarY;

	// Applied in PinCamera(), i.e. at render time, outside gadget input processing.
	PendingClick = Point2D { screenX, screenY };
	HasPendingClick = true;

	if (MoveLogCount < 5)
	{
		++MoveLogCount;
		Debug::Log("[Phobos::TacticalMap] click queued: radar={%d,%d} screen={%d,%d} rect={%d,%d,%d,%d}\n",
			radarX, radarY, screenX, screenY, src.X, src.Y, src.Width, src.Height);
	}

	return true;
}

void TacticalMap::HandleClickAt(const Point2D& point)
{
	if (!Opened || LastHandledFrame == Unsorted::CurrentFrame)
		return;

	if (HandleClick(point))
		LastHandledFrame = Unsorted::CurrentFrame;
}

void TacticalMap::ProcessGadgetInput(const Point2D& point)
{
	if (!Opened)
		return;

	PollHotkeys();

	if (!Opened)
		return;

	const bool left = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
	const bool right = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;

	const bool leftPressed = left && !PrevLeftDown;
	const bool rightPressed = right && !PrevRightDown;
	const bool leftReleased = !left && PrevLeftDown;

	RectangleStruct src {};
	RectangleStruct dst {};

	if (ComputeRects(src, dst))
	{
		bool handled = false;
		const RectangleStruct& view = DSurface::ViewBounds;
		const ToolbarLayout layout = ComputeToolbar(view, dst);

		if (rightPressed)
			ActivePopup = Popup::None;

		if (leftPressed && EditingText)
			CommitText();

		if (leftPressed)
		{
			// 1. An item in the open colour / width popup?
			if (ActivePopup != Popup::None)
			{
				const PopupLayout popup = ComputePopup(ActivePopup, layout, view);

				for (int i = 0; i < popup.Count; ++i)
				{
					if (!PointIn(point, popup.Items[i]))
						continue;

					if (ActivePopup == Popup::Colors)
					{
						ColorIndex = i;
						BrushRgb = Palette[i];
					}
					else
					{
						WidthIndex = i;
						BrushWidth = Widths[i];
					}

					ActivePopup = Popup::None;
					handled = true;
					break;
				}

				if (!handled)
				{
					if (PointIn(point, popup.Panel))
						handled = true;
					else
						ActivePopup = Popup::None;
				}
			}

			// 2. A toolbar button?
			if (!handled && PointIn(point, layout.Panel))
			{
				handled = true;

				for (int i = 0; i < 9; ++i)
				{
					if (!PointIn(point, layout.Buttons[i]))
						continue;

					switch (static_cast<ToolButton>(i))
					{
					case ToolButton::Camera:
					case ToolButton::Brush:
					case ToolButton::Eraser:
					case ToolButton::Ruler:
					case ToolButton::Text:
					case ToolButton::Marker:
						CurrentTool = static_cast<Tool>(i);

						if (StrokeActive)
							NetSendStrokeEnd();

						StrokeActive = false;
						CurrentStrokeOp = 0;
						MeasureActive = false;
						TacticalMapCanvas::EndStroke();
						ActivePopup = Popup::None;
						break;

					case ToolButton::Color:
						ActivePopup = ActivePopup == Popup::Colors ? Popup::None : Popup::Colors;
						break;

					case ToolButton::Width:
						ActivePopup = ActivePopup == Popup::Widths ? Popup::None : Popup::Widths;
						break;

					case ToolButton::Clear:
						ClearAll();
						StrokeActive = false;
						MeasureActive = false;
						ActivePopup = Popup::None;
						break;

					default:
						break;
					}

					if (ToolbarLogCount < 20)
					{
						++ToolbarLogCount;
						Debug::Log("[Phobos::TacticalMap] toolbar: button=%d tool=%d color=%06X width=%d popup=%d\n",
							i, static_cast<int>(CurrentTool), BrushRgb, BrushWidth, static_cast<int>(ActivePopup));
					}

					break;
				}
			}

			// 3. Any other click dismisses an open popup and is consumed.
			if (!handled && ActivePopup != Popup::None)
			{
				ActivePopup = Popup::None;
				handled = true;
			}
		}

		const bool inMap = PointIn(point, dst);

		if (!handled)
		{
			if (CurrentTool == Tool::Brush || CurrentTool == Tool::Eraser)
			{
				if (leftPressed && inMap)
					StartStroke(point, dst);
				else if (StrokeActive && left)
					ContinueStroke(point, dst);

				if (leftReleased && StrokeActive)
				{
					TacticalMapCanvas::EndStroke();
					StrokeActive = false;
					NetSendStrokeEnd();

					UndoAction action;
					action.Kind = UndoKind::Canvas;
					action.OpId = CurrentStrokeOp;
					CurrentStrokeOp = 0;

					if (TacticalMapCanvas::TakeStrokeEdits(action.Pixels))
						PushUndo(std::move(action));
				}
			}
			else if (CurrentTool == Tool::Ruler)
			{
				if (leftPressed && inMap)
				{
					MeasureActive = true;
					MeasureStart = ToNormalised(point, dst);
					MeasureEnd = MeasureStart;
				}
				else if (MeasureActive && left)
				{
					MeasureEnd = ToNormalised(point, dst);
				}

				if (leftReleased && MeasureActive)
				{
					MeasureActive = false;

					if (MeasureStart.X != MeasureEnd.X || MeasureStart.Y != MeasureEnd.Y)
					{
						MeasureLine line;
						line.A = MeasureStart;
						line.B = MeasureEnd;
						line.Id = NetNewOpId();
						Measures.push_back(line);
						NetSendMeasureAdd(Measures.back());

						UndoAction action;
						action.Kind = UndoKind::MeasureAdd;
						action.OpId = line.Id;
						action.MeasureIndex = static_cast<int>(Measures.size()) - 1;
						PushUndo(std::move(action));
					}
				}

				if (rightPressed && !Measures.empty())
				{
					UndoAction action;
					action.Kind = UndoKind::MeasureDelete;
					action.MeasureIndex = static_cast<int>(Measures.size()) - 1;
					action.Measure = Measures.back();
					action.OpId = NetNewOpId();
					NetSendMeasureDelete(action.OpId, action.Measure.Id);
					Measures.pop_back();
					PushUndo(std::move(action));
				}
			}
			else if (CurrentTool == Tool::Text)
			{
				if (leftPressed && inMap)
				{
					bool removed = false;

					for (auto it = Texts.begin(); it != Texts.end(); ++it)
					{
						RectangleStruct bounds = it->Bounds;
						bounds.X -= 3;
						bounds.Y -= 3;
						bounds.Width += 6;
						bounds.Height += 6;

						if (PointIn(point, bounds))
						{
							UndoAction action;
							action.Kind = UndoKind::TextDelete;
							action.TextIndex = static_cast<int>(it - Texts.begin());
							action.Text = *it;
							action.OpId = NetNewOpId();
							NetSendTextDelete(action.OpId, it->Id);
							Texts.erase(it);
							PushUndo(std::move(action));
							removed = true;
							break;
						}
					}

					if (!removed)
						BeginEdit(point, dst);
				}
			}
			else if (CurrentTool == Tool::Marker)
			{
				if (leftPressed && inMap)
				{
					bool removed = false;

					for (auto it = Markers.begin(); it != Markers.end(); ++it)
					{
						const int mx = dst.X + static_cast<int>(std::lround(NormX(it->Pos) * dst.Width));
						const int my = dst.Y + static_cast<int>(std::lround(NormY(it->Pos) * dst.Height));
						const int dx = point.X - mx;
						const int dy = point.Y - my;
						const int radius = std::max(10, it->Size);

						if (dx * dx + dy * dy <= radius * radius)
						{
							UndoAction action;
							action.Kind = UndoKind::MarkerDelete;
							action.MarkerIndex = static_cast<int>(it - Markers.begin());
							action.Marker = *it;
							action.OpId = NetNewOpId();
							NetSendMarkerDelete(action.OpId, it->Id);
							Markers.erase(it);
							PushUndo(std::move(action));
							removed = true;
							break;
						}
					}

					if (!removed)
					{
						MarkerItem marker;
						marker.Pos = ToNormalised(point, dst);
						marker.Rgb = BrushRgb;
						marker.Size = 8 + BrushWidth * 2;
						marker.Id = NetNewOpId();
						Markers.push_back(marker);
						NetSendMarkerAdd(Markers.back());

						UndoAction action;
						action.Kind = UndoKind::MarkerAdd;
						action.OpId = marker.Id;
						action.MarkerIndex = static_cast<int>(Markers.size()) - 1;
						PushUndo(std::move(action));
					}
				}
			}
			else if (CurrentTool == Tool::Camera && inMap && (leftPressed || rightPressed))
			{
				HandleClick(point);
			}
		}
	}

	PrevLeftDown = left;
	PrevRightDown = right;
}

bool TacticalMap::OnEscape()
{
	if (!Opened)
		return false;

	if (EditingText)
	{
		CancelText();
		return true;
	}

	Close();
	return true;
}

bool TacticalMap::IsEditingText()
{
	return Opened && EditingText;
}

bool TacticalMap::HandleWindowMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	if (!Opened)
		return false;

	// The game opens its in-game menu from the ESC key message, which arrives before
	// any per-frame polling. Consume it while the overlay is open: cancel the text
	// editor if it is active, otherwise close the overlay.
	if ((message == WM_KEYDOWN || message == WM_KEYUP || message == WM_SYSKEYDOWN) && wParam == VK_ESCAPE)
	{
		if (message == WM_KEYDOWN)
		{
			if (EditingText)
				CancelText();
			else
				Close();
		}

		return true;
	}

	// Same for the translated character, in case the menu reacts to WM_CHAR.
	if (message == WM_CHAR && wParam == 27)
		return true;

	if (!EditingText)
		return false;

	switch (message)
	{
	case WM_IME_STARTCOMPOSITION:
		ImeComposing = true;
		EditComposition.clear();
		return true;

	case WM_IME_COMPOSITION:
	{
		HIMC himc = ImmGetContext(hwnd);

		if (!himc)
			return true;

		if (lParam & GCS_RESULTSTR)
		{
			const LONG bytes = ImmGetCompositionStringW(himc, GCS_RESULTSTR, nullptr, 0);

			if (bytes > 0)
			{
				std::wstring result(static_cast<size_t>(bytes) / sizeof(wchar_t), L'\0');
				ImmGetCompositionStringW(himc, GCS_RESULTSTR, result.data(), bytes);
				AppendEditText(result);
			}

			EditComposition.clear();
			ImeComposing = false;
		}
		else if (lParam & GCS_COMPSTR)
		{
			const LONG bytes = ImmGetCompositionStringW(himc, GCS_COMPSTR, nullptr, 0);

			if (bytes > 0)
			{
				std::wstring composition(static_cast<size_t>(bytes) / sizeof(wchar_t), L'\0');
				ImmGetCompositionStringW(himc, GCS_COMPSTR, composition.data(), bytes);
				EditComposition = composition;
			}
			else
			{
				EditComposition.clear();
			}
		}

		ImmReleaseContext(hwnd, himc);
		return true;
	}

	case WM_IME_ENDCOMPOSITION:
		ImeComposing = false;
		EditComposition.clear();
		return true;

	case WM_IME_CHAR:
		// Committed characters are collected through WM_IME_COMPOSITION; ignoring
		// this avoids inserting every character twice.
		return true;

	case WM_CHAR:
		if (ImeComposing)
			return true;

		if (wParam == L'\r')
		{
			CommitText();
			return true;
		}

		if (wParam == L'\b')
		{
			if (!EditValue.empty())
				EditValue.pop_back();

			return true;
		}

		if (wParam >= 32 && wParam != 127)
			AppendEditText(std::wstring(1, static_cast<wchar_t>(wParam)));

		return true;

	case WM_KEYDOWN:
		if (ImeComposing)
			return true;

		if (wParam == VK_RETURN)
		{
			CommitText();
			return true;
		}

		if (wParam == VK_ESCAPE)
		{
			CancelText();
			return true;
		}

		if (wParam == VK_BACK)
		{
			if (!EditValue.empty())
				EditValue.pop_back();

			return true;
		}

		return true;

	default:
		break;
	}

	return false;
}

void TacticalMap::ResetCanvas()
{
	TacticalMapCanvas::Reset();
	Measures.clear();
	Texts.clear();
	Markers.clear();
	RemoteOps.clear();
	NetFragments.clear();
	RemoteStrokeActive = false;
	RemoteStrokeIgnored = false;
	RemoteStrokeOp = 0;
	CurrentStrokeOp = 0;
	StrokeActive = false;
	MeasureActive = false;
	CurrentTool = Tool::Camera;
	CancelText();
	UndoHistory.clear();
	UndoBytes = 0;
}

void TacticalMap::DebugComparePixel(int radarX, int radarY)
{
	RectangleStruct src {};
	RectangleStruct dst {};

	if (!ComputeRects(src, dst))
	{
		Debug::Log("[Phobos::TacticalMap] ours: radarPx={%d,%d} no rects\n", radarX, radarY);
		return;
	}

	int cellX = 0;
	int cellY = 0;

	if (!RadarToCell(src, radarX, radarY, cellX, cellY))
	{
		Debug::Log("[Phobos::TacticalMap] ours: radarPx={%d,%d} mapping failed\n", radarX, radarY);
		return;
	}

	const RectangleStruct& map = MapClass::Instance.MapRect;

	Debug::Log("[Phobos::TacticalMap] ours: radarPx={%d,%d} src={%d,%d,%d,%d} cell={%d,%d} clamped={%d,%d}\n",
		radarX, radarY, src.X, src.Y, src.Width, src.Height, cellX, cellY,
		std::clamp(cellX, map.X, map.X + map.Width - 1),
		std::clamp(cellY, map.Y, map.Y + map.Height - 1));
}

void TacticalMap::PinCamera()
{
	if (!Opened)
		return;

	// The engine still switches to its edge-scroll cursor when the physical mouse
	// touches the screen border. The camera is pinned and all HUD input is blocked,
	// so keep the normal arrow for the whole overlay.
	if (MouseClass::Instance.MouseCursorIndex != MouseCursorType::Default)
		MouseClass::Instance.SetCursor(MouseCursorType::Default, false);

	// A queued overlay click is forwarded to the engine's radar handler here, at
	// render time, so it cannot re-enter gadget input processing.
	if (HasPendingClick)
	{
		HasPendingClick = false;

		if (TacticalClass::Instance)
		{
			ApplyRadarClick(PendingClick);
			PinnedCamera = TacticalClass::Instance->TacticalCoord1;
			PinValid = true;
		}
	}

	if (!PinValid || !TacticalClass::Instance)
		return;

	const Point2D current = TacticalClass::Instance->TacticalCoord1;

	// Edge scrolling already moved the camera during this frame's input update;
	// restoring the pin before the render keeps the view frozen on screen.
	if (current.X == PinnedCamera.X && current.Y == PinnedCamera.Y)
		return;

	Point2D target = PinnedCamera;
	TacticalClass::Instance->SetTacticalPosition(&target);
	MapClass::Instance.MarkNeedsRedraw(2);
}

// The tactical map shares annotations as a private event type. The event is
// queued/sent/executed by the engine exactly like an order, so all peers see the
// same event stream (and therefore the same command counts) - but the handler
// only touches client-side drawing state, never the simulation.
DEFINE_HOOK(0x4C6CC8, EventClass_Execute_TacticalMapShare, 0x5)
{
	GET(EventClass*, pEvent, ESI);

	if (static_cast<BYTE>(pEvent->Type) == NetEventType)
		ReceiveNetEvent(*pEvent);

	return 0;
}

// Event payload size for serialisation. 0x41 is in the range Ares/CnCNet leave
// free; the engine's own size table has no entry for it, so all three places
// that compute an event's size must agree on the fixed payload length.
DEFINE_HOOK(0x64B6FE, EventClass_Size_TacticalMapShare, 0x6)
{
	GET(uint32_t, type, EDI);

	if ((type & 0xFF) == NetEventType)
	{
		R->EDX(static_cast<int>(NetPayloadSize));
		R->EBP(static_cast<int>(NetPayloadSize));
		return 0x64B71D;
	}

	return 0;
}

DEFINE_HOOK(0x64BE7D, EventClass_ReadSize_TacticalMapShare, 0x6)
{
	GET(uint32_t, type, EDI);

	if ((type & 0xFF) == NetEventType)
	{
		REF_STACK(int, eventSize, STACK_OFFSET(0xAC, -0x8C));
		eventSize = static_cast<int>(NetPayloadSize);
		R->ECX(static_cast<int>(NetPayloadSize));
		R->EBP(static_cast<int>(NetPayloadSize));
		return 0x64BE97;
	}

	return 0;
}

DEFINE_HOOK(0x64C30E, EventClass_ParseSize_TacticalMapShare, 0x6)
{
	GET(uint32_t, type, ESI);

	if ((type & 0xFF) == NetEventType)
	{
		R->ECX(static_cast<int>(NetPayloadSize));
		R->EBP(static_cast<int>(NetPayloadSize));
		return 0x64C321;
	}

	return 0;
}
