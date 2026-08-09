// Tests: 2D/3D collision + spline evaluation math (no window needed)
#include "raylib.h"
#include "testkit.h"

int main(void)
{
    SetTraceLogLevel(LOG_WARNING);

    // 2D rectangle collisions
    Rectangle a = { 0, 0, 10, 10 }, b = { 5, 5, 10, 10 }, c = { 20, 20, 5, 5 };
    CHECK(CheckCollisionRecs(a, b));
    CHECK(!CheckCollisionRecs(a, c));
    Rectangle inter = GetCollisionRec(a, b);
    CHECK_NEAR(inter.x, 5, 1e-6); CHECK_NEAR(inter.width, 5, 1e-6);

    // Circles
    CHECK(CheckCollisionCircles((Vector2){ 0, 0 }, 5, (Vector2){ 7, 0 }, 3));
    CHECK(!CheckCollisionCircles((Vector2){ 0, 0 }, 2, (Vector2){ 7, 0 }, 3));
    CHECK(CheckCollisionCircleRec((Vector2){ -1, 5 }, 2, a));
    CHECK(CheckCollisionCircleLine((Vector2){ 5, 1 }, 2, (Vector2){ 0, 0 }, (Vector2){ 10, 0 }));

    // Points
    CHECK(CheckCollisionPointRec((Vector2){ 5, 5 }, a));
    CHECK(!CheckCollisionPointRec((Vector2){ 15, 5 }, a));
    CHECK(CheckCollisionPointCircle((Vector2){ 1, 1 }, (Vector2){ 0, 0 }, 2));
    CHECK(CheckCollisionPointTriangle((Vector2){ 1, 1 }, (Vector2){ 0, 0 }, (Vector2){ 0, 4 }, (Vector2){ 4, 0 }));
    CHECK(CheckCollisionPointLine((Vector2){ 5, 0 }, (Vector2){ 0, 0 }, (Vector2){ 10, 0 }, 1));
    Vector2 poly[4] = { { 0, 0 }, { 0, 10 }, { 10, 10 }, { 10, 0 } };
    CHECK(CheckCollisionPointPoly((Vector2){ 5, 5 }, poly, 4));
    CHECK(!CheckCollisionPointPoly((Vector2){ 15, 5 }, poly, 4));

    // Line-line
    Vector2 hit = { 0 };
    CHECK(CheckCollisionLines((Vector2){ 0, 0 }, (Vector2){ 10, 10 },
                              (Vector2){ 0, 10 }, (Vector2){ 10, 0 }, &hit));
    CHECK_NEAR(hit.x, 5, 1e-4); CHECK_NEAR(hit.y, 5, 1e-4);

    // Splines
    Vector2 lin = GetSplinePointLinear((Vector2){ 0, 0 }, (Vector2){ 10, 0 }, 0.5f);
    CHECK_NEAR(lin.x, 5, 1e-4);
    Vector2 bq = GetSplinePointBezierQuad((Vector2){ 0, 0 }, (Vector2){ 5, 10 }, (Vector2){ 10, 0 }, 0.5f);
    CHECK_NEAR(bq.x, 5, 1e-4); CHECK_NEAR(bq.y, 5, 1e-4);
    Vector2 bc = GetSplinePointBezierCubic((Vector2){ 0, 0 }, (Vector2){ 0, 10 }, (Vector2){ 10, 10 }, (Vector2){ 10, 0 }, 0.5f);
    CHECK_NEAR(bc.x, 5, 1e-4); CHECK_NEAR(bc.y, 7.5, 1e-4);

    // 3D spheres/boxes
    CHECK(CheckCollisionSpheres((Vector3){ 0, 0, 0 }, 2, (Vector3){ 3, 0, 0 }, 2));
    BoundingBox bb1 = { { 0, 0, 0 }, { 2, 2, 2 } };
    BoundingBox bb2 = { { 1, 1, 1 }, { 3, 3, 3 } };
    BoundingBox bb3 = { { 5, 5, 5 }, { 6, 6, 6 } };
    CHECK(CheckCollisionBoxes(bb1, bb2));
    CHECK(!CheckCollisionBoxes(bb1, bb3));
    CHECK(CheckCollisionBoxSphere(bb1, (Vector3){ 3, 1, 1 }, 1.5f));

    // Ray casts
    Ray ray = { .position = { 0, 0, -5 }, .direction = { 0, 0, 1 } };
    RayCollision rc = GetRayCollisionSphere(ray, (Vector3){ 0, 0, 0 }, 1.0f);
    CHECK(rc.hit);
    CHECK_NEAR(rc.distance, 4.0, 1e-3);
    RayCollision rb = GetRayCollisionBox(ray, (BoundingBox){ { -1, -1, -1 }, { 1, 1, 1 } });
    CHECK(rb.hit);
    CHECK_NEAR(rb.distance, 4.0, 1e-3);
    RayCollision rt = GetRayCollisionTriangle(ray, (Vector3){ -1, -1, 0 }, (Vector3){ 1, -1, 0 }, (Vector3){ 0, 2, 0 });
    CHECK(rt.hit);
    CHECK_NEAR(rt.distance, 5.0, 1e-3);
    RayCollision rq = GetRayCollisionQuad(ray, (Vector3){ -1, -1, 0 }, (Vector3){ -1, 1, 0 }, (Vector3){ 1, 1, 0 }, (Vector3){ 1, -1, 0 });
    CHECK(rq.hit);
    // raylib parity quirk: a sphere behind the ray still reports hit=true,
    // with a negative distance (upstream does not reject vector < 0)
    Ray miss = { .position = { 0, 0, -5 }, .direction = { 0, 0, -1 } };
    RayCollision behind = GetRayCollisionSphere(miss, (Vector3){ 0, 0, 0 }, 1.0f);
    CHECK(!behind.hit || (behind.distance < 0.0f));

    return tk_report("test_collision");
}
