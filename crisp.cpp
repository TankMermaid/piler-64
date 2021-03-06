#include "piler2.h"
#include "bitfuncs.h"
#include <algorithm>

#define TRACE		0

#define GFFRecord GFFRecord2

/***
Find CRISPR families.

CRISPR candidate:

              <-----spacer------->

    ------====--------------------====------------ genome

	      Pile                    Pile
           ^-----------------------^
                      Hit

Candidate if
    (a) hit length >= MIN_LENGTH_CRISPR and <= MAX_LENGTH_CRISPR, and
    (b) offset is >= MIN_LENGTH_SPACER and <= MAX_LENGTH_SPACER.

***/

static int MIN_CRISPR_LENGTH = 10;
static int MAX_CRISPR_LENGTH = 200;
static int MIN_SPACER_LENGTH = 10;
static int MAX_SPACER_LENGTH = 200;
static double MIN_CRISPR_RATIO = 0.8;
static double MIN_SPACER_RATIO = 0.8;
static int MIN_FAM_SIZE = 3;
static int MAX_SPACE_DIFF = 20;

static int g_paramMinFamSize = 3;
static int g_paramMaxLengthDiffPct = 5;
static bool g_paramSingleHitCoverage = true;

static PileData *Piles;
static int PileCount;
static int EdgeCount;

static int MaxImageCount = 0;
static int SeqLength;
static int SeqLengthChunks;

static int GetHitLength(const HitData &Hit)
	{
	const int QueryHitLength = Hit.QueryTo - Hit.QueryFrom + 1;
	const int TargetHitLength = Hit.TargetTo - Hit.TargetFrom + 1;
	return (QueryHitLength + TargetHitLength)/2;
	}

static int GetSpacerLength(const HitData &Hit)
	{
	if (Hit.QueryFrom > Hit.TargetTo)
		return Hit.QueryFrom - Hit.TargetTo;
	else
		return Hit.TargetFrom - Hit.QueryTo;
	}

static bool IsCand(const HitData &Hit)
	{
	if (Hit.Rev)
		return false;

	int HitLength = GetHitLength(Hit);
	if (HitLength > MAX_CRISPR_LENGTH || HitLength < MIN_CRISPR_LENGTH)
		return false;

	int SpacerLength = GetSpacerLength(Hit);
	if (SpacerLength > MAX_SPACER_LENGTH || SpacerLength < MIN_SPACER_LENGTH)
		return false;

	return true;
	}

static PILE_INDEX_TYPE *IdentifyPiles(int *CopyCount)
	{
	PILE_INDEX_TYPE *PileIndexes = all(PILE_INDEX_TYPE, SeqLengthChunks);

#if	DEBUG
	memset(PileIndexes, 0xff, SeqLengthChunks*sizeof(PILE_INDEX_TYPE));
#endif

	int PileIndex = -1;
	bool InPile = false;
	for (int i = 0; i < SeqLengthChunks; ++i)
		{
		if (BitIsSet(CopyCount, i))
			{
			if (!InPile)
				{
				++PileIndex;
				if (PileIndex > MAX_STACK_INDEX)
					Quit("Too many stacks");
				InPile = true;
				}
			PileIndexes[i] = PileIndex;
			}
		else
			InPile = false;
		}
	PileCount = PileIndex + 1;
	return PileIndexes;
	}

static void IncCopyCountImage(int *CopyCount, int From, int To)
	{
	if (From < 0)
		Quit("From < 0");

	From /= CHUNK_LENGTH;
	To /= CHUNK_LENGTH;

	if (From >= SeqLengthChunks)
		{
		Warning("IncCopyCountImage: From=%d, SeqLength=%d", From, SeqLengthChunks);
		From = SeqLengthChunks - 1;
		}
	if (To >= SeqLengthChunks)
		{
		Warning("IncCopyCountImage: To=%d, SeqLength=%d", To, SeqLengthChunks);
		To = SeqLengthChunks - 1;
		}

	if (From > To)
		Quit("From > To");

	for (int i = From; i <= To; ++i)
		SetBit(CopyCount, i);
	}

static void IncCopyCount(int *CopyCount, const HitData &Hit)
	{
	IncCopyCountImage(CopyCount, Hit.TargetFrom, Hit.TargetTo);
	IncCopyCountImage(CopyCount, Hit.QueryFrom, Hit.QueryTo);
	}

static int CmpHits(const void *ptrHit1, const void *ptrHit2)
	{
	HitData *Hit1 = (HitData *) ptrHit1;
	HitData *Hit2 = (HitData *) ptrHit2;
	return Hit1->QueryFrom - Hit2->QueryFrom;
	}

static int CmpImages(const void *ptrImage1, const void *ptrImage2)
	{
	PileImageData *Image1 = (PileImageData *) ptrImage1;
	PileImageData *Image2 = (PileImageData *) ptrImage2;
	return Image1->SIPile - Image2->SIPile;
	}

static void AssertImagesSorted(PileImageData *Images, int ImageCount)
	{
	for (int i = 0; i < ImageCount - 1; ++i)
		if (Images[i].SIPile > Images[i+1].SIPile)
			Quit("Images not sorted");
	}

static void SortImagesPile(PileImageData *Images, int ImageCount)
	{
	qsort(Images, ImageCount, sizeof(PileImageData), CmpImages);
	}

static void SortImages()
	{
	for (int PileIndex = 0; PileIndex < PileCount; ++PileIndex)
		{
		PileData &Pile = Piles[PileIndex];
		SortImagesPile(Pile.Images, Pile.ImageCount);
#if	DEBUG
		AssertImagesSorted(Pile.Images, Pile.ImageCount);
#endif
		}
	}

static bool FamMemberLt(const FamMemberData &Mem1, const FamMemberData &Mem2)
	{
	const PileData &Pile1 = Piles[Mem1.PileIndex];
	const PileData &Pile2 = Piles[Mem2.PileIndex];
	return Pile1.From < Pile2.From;
	}

static void CreatePiles(const HitData *Hits, int HitCount,
  PILE_INDEX_TYPE *PileIndexes)
	{
	Piles = all(PileData, PileCount);
	zero(Piles, PileData, PileCount);
	for (int i = 0; i < PileCount; ++i)
		{
		Piles[i].FamIndex = -1;
		Piles[i].SuperFamIndex = -1;
		Piles[i].Rev = -1;
		}

// Count images in stack
	ProgressStart("Create piles: count images");
	for (int HitIndex = 0; HitIndex < HitCount; ++HitIndex)
		{
		const HitData &Hit = Hits[HitIndex];

		int Pos = Hit.QueryFrom/CHUNK_LENGTH;
		PILE_INDEX_TYPE PileIndex = PileIndexes[Pos];
		assert(PileIndex == PileIndexes[Hit.QueryTo/CHUNK_LENGTH]);
		assert(PileIndex >= 0 && PileIndex < PileCount);
		++(Piles[PileIndex].ImageCount);

		Pos = Hit.TargetFrom/CHUNK_LENGTH;
		PileIndex = PileIndexes[Pos];
		assert(PileIndex >= 0 && PileIndex < PileCount);
		assert(PileIndex == PileIndexes[Hit.TargetTo/CHUNK_LENGTH]);
		++(Piles[PileIndex].ImageCount);
		}
	ProgressDone();

// Allocate memory for image list
	int TotalImageCount = 0;
	ProgressStart("Create piles: allocate image memory");
	for (int PileIndex = 0; PileIndex < PileCount; ++PileIndex)
		{
		PileData &Pile = Piles[PileIndex];
		const int ImageCount = Pile.ImageCount;
		TotalImageCount += ImageCount;
		assert(ImageCount > 0);
		Pile.Images = all(PileImageData, ImageCount);
		}
	ProgressDone();

// Build image list
	ProgressStart("Create piles: build image list");
	for (int PileIndex = 0; PileIndex < PileCount; ++PileIndex)
		{
		PileData &Pile = Piles[PileIndex];
		Pile.ImageCount = 0;
		Pile.From = -1;
		Pile.To = -1;
		}

	for (int HitIndex = 0; HitIndex < HitCount; ++HitIndex)
		{
		const HitData &Hit = Hits[HitIndex];

		const bool Rev = Hit.Rev;

		const int Length1 = Hit.QueryTo - Hit.QueryFrom;
		const int Length2 = Hit.TargetTo - Hit.TargetFrom;

		const int From1 = Hit.QueryFrom;
		const int From2 = Hit.TargetFrom;

		const int To1 = Hit.QueryTo;
		const int To2 = Hit.TargetTo;

		const int Pos1 = From1/CHUNK_LENGTH;
		const int Pos2 = From2/CHUNK_LENGTH;

		PILE_INDEX_TYPE PileIndex1 = PileIndexes[Pos1];
		PILE_INDEX_TYPE PileIndex2 = PileIndexes[Pos2];

		assert(PileIndex1 == PileIndexes[(From1 + Length1 - 1)/CHUNK_LENGTH]);
		assert(PileIndex1 >= 0 && PileIndex1 < PileCount);

		assert(PileIndex2 == PileIndexes[(From2 + Length2 - 1)/CHUNK_LENGTH]);
		assert(PileIndex2 >= 0 && PileIndex2 < PileCount);

		PileData &Pile1 = Piles[PileIndex1];
		PileImageData &Image1 = Pile1.Images[Pile1.ImageCount++];
		Image1.SILength = Length2;
		Image1.SIPile = PileIndex2;
		Image1.SIRev = Rev;

		PileData &Pile2 = Piles[PileIndex2];
		PileImageData &Image2 = Pile2.Images[Pile2.ImageCount++];
		Image2.SILength = Length1;
		Image2.SIPile = PileIndex1;
		Image2.SIRev = Rev;

		if (Pile1.From == -1 || From1 < Pile1.From)
			Pile1.From = From1;
		if (Pile1.To == -1 || To1 > Pile1.To)
			Pile1.To = To1;

		if (Pile2.From == -1 || From2 < Pile2.From)
			Pile2.From = From2;
		if (Pile2.To == -1 || To2 > Pile2.To)
			Pile2.To = To2;

		if (Pile1.ImageCount > MaxImageCount)
			MaxImageCount = Pile1.ImageCount;
		if (Pile2.ImageCount > MaxImageCount)
			MaxImageCount = Pile2.ImageCount;
		}
	ProgressDone();
	}

static int PileDist(const PileData &Pile1, const PileData &Pile2)
	{
	if (Pile1.From > Pile2.To)
		return Pile1.From - Pile2.To;
	else
		return Pile2.From - Pile1.From;
	}

static int FindEdgesPile(PileData &Pile, int PileIndex,
  PILE_INDEX_TYPE Partners[], bool PartnersRev[])
	{
	const int PileLength = Pile.To - Pile.From + 1;
	if (PileLength < MIN_CRISPR_LENGTH || PileLength > MAX_CRISPR_LENGTH)
		return 0;

	const int ImageCount = Pile.ImageCount;

	int PartnerCount = 0;
	for (int ImageIndex = 0; ImageIndex < ImageCount; ++ImageIndex)
		{
		const PileImageData &Image = Pile.Images[ImageIndex];
		const int PartnerImageLength = Image.SILength;
		const int PartnerPileIndex = Image.SIPile;
		const PileData &PartnerPile = Piles[PartnerPileIndex];
		const int PartnerPileLength = PartnerPile.To - PartnerPile.From + 1;
		const int Dist = PileDist(Pile, PartnerPile);

		if (PartnerPileLength >= MIN_CRISPR_LENGTH &&
		  PartnerPileLength <= MAX_CRISPR_LENGTH &&
		  Dist >= MIN_SPACER_LENGTH &&
		  Dist <= MAX_SPACER_LENGTH)
			{
			PartnersRev[PartnerCount] = Image.SIRev;
			Partners[PartnerCount] = PartnerPileIndex;
			++PartnerCount;
			}
		}
	return PartnerCount;
	}

static void AddEdges(EdgeList &Edges, PILE_INDEX_TYPE PileIndex,
  PILE_INDEX_TYPE Partners[], bool PartnersRev[], int PartnerCount)
	{
	EdgeCount += PartnerCount;
	for (int i = 0; i < PartnerCount; ++i)
		{
		int PileIndex2 = Partners[i];
		EdgeData Edge;
		Edge.Node1 = PileIndex;
		Edge.Node2 = PileIndex2;
		Edge.Rev = PartnersRev[i];
		Edges.push_back(Edge);
		}
	}

static void FindCandEdges(EdgeList &Edges, int MaxImageCount)
	{
	Edges.clear();

	PILE_INDEX_TYPE *Partners = all(PILE_INDEX_TYPE, MaxImageCount);
	bool *PartnersRev = all(bool, MaxImageCount);
	for (int PileIndex = 0; PileIndex < PileCount; ++PileIndex)
		{
		PileData &Pile = Piles[PileIndex];
		int PartnerCount;
		PartnerCount = FindEdgesPile(Pile, PileIndex, Partners, PartnersRev);
		AddEdges(Edges, PileIndex, Partners, PartnersRev, PartnerCount);
		}
	freemem(Partners);
	freemem(PartnersRev);
	}

static void AssignFamsToPiles(FamList &Fams)
	{
	int FamIndex = 0;
	for (PtrFamList p = Fams.begin(); p != Fams.end(); ++p)
		{
		FamData *Fam = *p;
		if (Fam->size() < (size_t) g_paramMinFamSize)
			Quit("Fam size");
		for (PtrFamData q = Fam->begin(); q != Fam->end(); ++q)
			{
			FamMemberData &FamMember = *q;
			int PileIndex = FamMember.PileIndex;
			PileData &Pile = Piles[PileIndex];
			Pile.FamIndex = FamIndex;
			Pile.Rev = (int) FamMember.Rev;
			}
		++FamIndex;
		}
	}

// Discard families that don't have regular spacing
typedef std::vector<FamMemberData> FAMVEC;

static void FilterCrispFam(const FamData &Fam, FamList &OutFams)
	{
	int PileCount = (int) Fam.size();

	FAMVEC FamVec;
	FamVec.reserve(PileCount);

	for (FamData::const_iterator p = Fam.begin(); p != Fam.end(); ++p)
		FamVec.push_back(*p);

	std::sort(FamVec.begin(), FamVec.end(), FamMemberLt);

#if	TRACE
	Log("\n");
	Log("FilterCrispFam fam size=%d\n", (int) Fam.size());
	Log("\n");
	Log("After sort:\n");
	Log(" Pile     From       To\n");
	Log("=====  =======  =======\n");
	for (int i = 0; i < PileCount; ++i)
		{
		const FamMemberData &Mem = FamVec[i];
		int PileIndex = Mem.PileIndex;
		const PileData &Pile = Piles[PileIndex];
		Log("%5d  %7d  %7d\n", PileIndex, Pile.From, Pile.To);
		}
	Log("\n");
#endif

	FamData OutFam;
	for (int i = 0; i < PileCount; ++i)
		{
		const FamMemberData &Mem = FamVec[i];

		if (i == 0 || i == 1)
			{
			OutFam.push_back(Mem);
			continue;
			}

		const FamMemberData &Mem_1 = FamVec[i-1];
		const FamMemberData &Mem_2 = FamVec[i-2];

		int PileIndex = Mem.PileIndex;
		int PileIndex_1 = Mem_1.PileIndex;
		int PileIndex_2 = Mem_2.PileIndex;

		const PileData &Pile = Piles[PileIndex];
		const PileData &Pile_1 = Piles[PileIndex_1];
		const PileData &Pile_2 = Piles[PileIndex_2];

		int Space_12 = Pile_1.From - Pile_2.To;
		int Space_1 = Pile.From - Pile_1.To;
#if	TRACE
		{
		Log("<Pile %d %d-%d> <space %d> <Pile %d %d-%d> <space %d> <Pile %d %d-%d>\n",
		  PileIndex_2,
		  Pile_2.From,
		  Pile_2.To,
		  Space_12,
		  PileIndex_1,
		  Pile_1.From,
		  Pile_2.To,
		  Space_1,
		  PileIndex,
		  Pile.From,
		  Pile.To);
		}
#endif

		if (iabs(Space_12 - Space_1) <= MAX_SPACE_DIFF)
			{
#if	TRACE
			Log("Add %d to current family\n", PileIndex);
#endif
			OutFam.push_back(Mem);
			}
		else
			{
#if	TRACE
			Log("Space difference too big, fam size so far %d\n", (int) OutFam.size());
#endif
			if (OutFam.size() >= (size_t) g_paramMinFamSize)
				{
				FamData *NewFam = new FamData;
				*NewFam = OutFam;
				OutFams.push_back(NewFam);
				OutFam = *new FamData;
				}
			else
				OutFam.clear();
			}
		}

	if (OutFam.size() >= (size_t) g_paramMinFamSize)
		{
		FamData *NewFam = new FamData;
		*NewFam = OutFam;
		OutFams.push_back(NewFam);
		}
	}

static void FilterCrispFams(const FamList &InFams, FamList &OutFams)
	{
	OutFams.clear();

	for (FamList::const_iterator p = InFams.begin(); p != InFams.end(); ++p)
		{
		FamData *Fam = *p;
		FilterCrispFam(*Fam, OutFams);
		}
	}

void Crisp()
	{
	const char *InputFileName = RequiredValueOpt("crisp");

	const char *OutputFileName = ValueOpt("out");
	const char *PilesFileName = ValueOpt("piles");
	const char *ImagesFileName = ValueOpt("images");
	const char *ArraysFileName = ValueOpt("arrays");

	const char *strMinFamSize = ValueOpt("famsize");

	if (0 == OutputFileName && 0 == PilesFileName && 0 == ImagesFileName)
		Quit("No output file specified, must be at least one of -out, -piles, -images");

	if (0 != strMinFamSize)
		g_paramMinFamSize = atoi(strMinFamSize);

	ProgressStart("Read hit file");
	int HitCount;
	int SeqLength;
	HitData *Hits = ReadHits(InputFileName, &HitCount, &SeqLength);
	ProgressDone();
	Progress("%d hits", HitCount);

	ProgressStart("Filter candidate hits");
	int NewHitCount = 0;
	for (int i = 0; i < HitCount; ++i)
		{
		HitData &Hit = Hits[i];
		if (IsCand(Hit))
			Hits[NewHitCount++] = Hit;
		}
	ProgressDone();
	Progress("%d of %d candidate hits", NewHitCount, HitCount);
	HitCount = NewHitCount;

	SeqLengthChunks = (SeqLength + CHUNK_LENGTH - 1)/CHUNK_LENGTH;

	const int BitVectorLength = (SeqLengthChunks + BITS_PER_INT - 1)/BITS_PER_INT;
	int *CopyCount = all(int, BitVectorLength);
	zero(CopyCount, int, BitVectorLength);

	ProgressStart("Compute copy counts");
	for (int i = 0; i < HitCount; ++i)
		IncCopyCount(CopyCount, Hits[i]);
	ProgressDone();

	ProgressStart("Identify piles");
	PILE_INDEX_TYPE *PileIndexes = IdentifyPiles(CopyCount);
	ProgressDone();

	Progress("%d stacks", PileCount);

	freemem(CopyCount);
	CopyCount = 0;

	CreatePiles(Hits, HitCount, PileIndexes);

	if (0 != ImagesFileName)
		{
		ProgressStart("Writing images file");
		WriteImages(ImagesFileName, Hits, HitCount, PileIndexes);
		ProgressDone();
		}

	freemem(Hits);
	Hits = 0;

	if (0 != PilesFileName)
		{
		ProgressStart("Writing piles file");
		WritePiles(PilesFileName, Piles, PileCount);
		ProgressDone();
		}

	freemem(PileIndexes);
	PileIndexes = 0;

	if (0 == OutputFileName)
		return;

	ProgressStart("Find edges");
	EdgeList Edges;
	FindCandEdges(Edges, MaxImageCount);
	ProgressDone();

	Progress("%d edges", (int) Edges.size());

	Progress("Find connected components");
	FamList Fams;
	FindConnectedComponents(Edges, Fams, g_paramMinFamSize);

	Progress("Filter");
	FamList OutFams;
	FilterCrispFams(Fams, OutFams);

	AssignFamsToPiles(OutFams);
	ProgressDone();

	Progress("%d arrays", (int) OutFams.size());

	ProgressStart("Write crisp file");
	WriteCrispFile(OutputFileName, Piles, PileCount);
	ProgressDone();

	if (0 != ArraysFileName)
		{
		FILE *fArray = OpenStdioFile(ArraysFileName, FILEIO_MODE_WriteOnly);
		ProgressStart("Writing arrays file");
		int FamIndex = 0;
		for (PtrFamList p = OutFams.begin(); p != OutFams.end(); ++p)
			{
			int Lo = -1;
			int Hi = -1;
			FamData *Fam = *p;
			if (Fam->size() < (size_t) g_paramMinFamSize)
				Quit("Fam size");
			for (PtrFamData q = Fam->begin(); q != Fam->end(); ++q)
				{
				FamMemberData &FamMember = *q;
				int PileIndex = FamMember.PileIndex;
				PileData &Pile = Piles[PileIndex];
				if (Lo == -1 || Pile.From < Lo)
					Lo = Pile.From;
				if (Hi == -1 || Pile.To > Hi)
					Hi = Pile.To;
				}
			WriteArray(fArray, FamIndex, Lo, Hi);
			++FamIndex;
			}
		fclose(fArray);
		ProgressDone();
		}
	}
