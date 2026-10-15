#include "Combat/Y01Targeting.h"
#include "Combat/Y01Combat.h"
#include "GameFramework/Actor.h"

bool Y01Targeting::InRadiusXY(const UY01CombatAbilitySystem* Source, const AActor* Target, double Radius)
{
    if (!Source || !FMath::IsFinite(Radius) || Radius <= 0 || !Source->IsLegalCombatTarget(Target)) { return false; }
    const double Distance = FVector::DistSquared2D(Source->GetAvatarActor()->GetActorLocation(), Target->GetActorLocation());
    return FMath::IsFinite(Distance) && Distance <= FMath::Square(Radius);
}
AActor* Y01Targeting::NearestXY(const UY01CombatAbilitySystem* Source, TConstArrayView<AActor*> Candidates, double Radius)
{
    AActor* Nearest = nullptr; double Best = TNumericLimits<double>::Max();
    for (AActor* Candidate : Candidates)
    {
        if (!InRadiusXY(Source, Candidate, Radius)) { continue; }
        const double Distance = FVector::DistSquared2D(Source->GetAvatarActor()->GetActorLocation(), Candidate->GetActorLocation());
        if (Distance < Best || (Distance == Best && (!Nearest || Candidate->GetPathName() < Nearest->GetPathName())))
        { Best = Distance; Nearest = Candidate; }
    }
    return Nearest;
}
